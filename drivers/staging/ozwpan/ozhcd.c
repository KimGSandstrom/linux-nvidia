/* -----------------------------------------------------------------------------
 * Copyright (c) 2011 Ozmo Inc
 * Released under the GNU General Public License Version 2 (GPLv2).
 *
 * This file provides the implementation of a USB host controller device that
 * does not have any associated hardware. Instead the virtual device is
 * connected to the WiFi network and emulates the operation of a USB hcd by
 * receiving and sending network frames.
 * Note:
 * We take great pains to reduce the amount of code where interrupts need to be
 * disabled and in this respect we are different from standard HCD's. In
 * particular we don't want in_irq() code bleeding over to the protocol side of
 * the driver.
 * The troublesome functions are the urb enqueue and dequeue functions both of
 * which can be called in_irq(). So for these functions we put the urbs into a
 * queue and request a tasklet to process them. This means that a spinlock with
 * interrupts disabled must be held for insertion and removal but most code is
 * is in tasklet or soft irq context. The lock that protects this list is called
 * the tasklet lock and serves the purpose of the 'HCD lock' which must be held
 * when calling the following functions.
 *   usb_hcd_link_urb_to_ep()
 *   usb_hcd_unlink_urb_from_ep()
 *   usb_hcd_flush_endpoint()
 *   usb_hcd_check_unlink_urb()
 * -----------------------------------------------------------------------------
 */
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/export.h>
#include "linux/usb/hcd.h"
#include <asm/unaligned.h>
#include "ozusbif.h"
#include "oztrace.h"
#include "ozurbparanoia.h"
#include "ozhcd.h"
#include "ozpd.h"
#include "ozproto.h"
#include "ozusbsvc.h"

#define OZ_HUB_DEBOUNCE_TIMEOUT	1500

/* Name of our platform device.
 */
#define OZ_PLAT_DEV_NAME	"ozwpan"
/* Maximum number of free urb links that can be kept in the pool.
 */
#define OZ_MAX_LINK_POOL_SIZE	16
/* Get endpoint object from the containing link.
 */
#define ep_from_link(__e) container_of((__e), struct oz_endpoint, link)
/*EP0 timeout before ep0 request is again added to TX queue. (13*8 = 98mSec)
 */
#define EP0_TIMEOUT_COUNTER 13
/*------------------------------------------------------------------------------
 * Used to link urbs together and also store some status information for each
 * urb.
 * A cache of these are kept in a pool to reduce number of calls to kmalloc.
 */
struct oz_urb_link {
	struct list_head link;
	struct urb *urb;
	struct oz_port *port;
	u8 req_id;
	u8 ep_num;
	unsigned submit_counter;
};

/* Holds state information about a USB endpoint.
 */
struct oz_endpoint {
	struct list_head urb_list;	/* List of oz_urb_link items. */
	struct list_head link;		/* For isoc ep, links in to isoc
					   lists of oz_port. */
	struct timespec timestamp;
	int credit2;
	int credit;
	int credit_ceiling;
	u8 ep_num;
	u8 attrib;
	u8 *buffer;
	int buffer_size;
	int in_ix;
	int out_ix;
	int buffered_units;
	u8 max_buffer_units;
	unsigned flags;
	int start_frame;
};
/* Bits in the flags field. */
#define OZ_F_EP_BUFFERING	0x1
#define OZ_F_EP_HAVE_STREAM	0x2


/* Buffer_size.
Total size of buffer (in bytes) for the endpoint buffer for isochronous data,
and for stashing BULK or INT data if a URB is not available.
*/
#define OZ_EP_BUFFER_SIZE_ISOC  (1024*24)
#define OZ_EP_BUFFER_SIZE_INT   (512)
/* Holds state information about a USB interface.
 */
struct oz_interface {
	unsigned ep_mask;
	u8 alt;
};

/* Holds state information about an hcd port.
 */
#define OZ_NB_ENDPOINTS	16
struct oz_port {
	unsigned flags;
	unsigned status;
	void *hpd;
	struct oz_hcd *ozhcd;
	spinlock_t port_lock;
	u8 bus_addr;
	u8 next_req_id;
	u8 config_num;
	int num_iface;
	struct oz_interface *iface;
	struct oz_endpoint *out_ep[OZ_NB_ENDPOINTS];
	struct oz_endpoint *in_ep[OZ_NB_ENDPOINTS];
	struct list_head isoc_out_ep;
	struct list_head isoc_in_ep;
};
#define OZ_PORT_F_PRESENT	0x1
#define OZ_PORT_F_CHANGED	0x2
#define OZ_PORT_F_DYING		0x4

/* Data structure in the private context area of struct usb_hcd.
 */
#define OZ_NB_PORTS	8
struct oz_hcd {
	spinlock_t hcd_lock;
	struct list_head urb_pending_list;
	struct list_head urb_cancel_list;
	struct list_head orphanage;
	int conn_port; /* Port that is currently connecting, -1 if none.*/
	struct oz_port ports[OZ_NB_PORTS];
	uint flags;
	struct usb_hcd *hcd;
};
/* Bits in flags field.
 */
#define OZ_HDC_F_SUSPENDED	0x1

/*------------------------------------------------------------------------------
 * Static function prototypes.
 */
static int oz_hcd_start(struct usb_hcd *hcd);
static void oz_hcd_stop(struct usb_hcd *hcd);
static void oz_hcd_shutdown(struct usb_hcd *hcd);
static int oz_hcd_urb_enqueue(struct usb_hcd *hcd, struct urb *urb,
				gfp_t mem_flags);
static int oz_hcd_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status);
static void oz_hcd_endpoint_disable(struct usb_hcd *hcd,
				struct usb_host_endpoint *ep);
static void oz_hcd_endpoint_reset(struct usb_hcd *hcd,
				struct usb_host_endpoint *ep);
static int oz_hcd_get_frame_number(struct usb_hcd *hcd);
static int oz_hcd_hub_status_data(struct usb_hcd *hcd, char *buf);
static int oz_hcd_hub_control(struct usb_hcd *hcd, u16 req_type, u16 wvalue,
				u16 windex, char *buf, u16 wlength);
static int oz_hcd_bus_suspend(struct usb_hcd *hcd);
static int oz_hcd_bus_resume(struct usb_hcd *hcd);
static int oz_plat_probe(struct platform_device *dev);
static int oz_plat_remove(struct platform_device *dev);
static void oz_plat_shutdown(struct platform_device *dev);
static int oz_plat_suspend(struct platform_device *dev, pm_message_t msg);
static int oz_plat_resume(struct platform_device *dev);
static void oz_urb_process_tasklet(unsigned long unused);
static int oz_build_endpoints_for_config(struct usb_hcd *hcd,
		struct oz_port *port, struct usb_host_config *config,
		gfp_t mem_flags);
static void oz_clean_endpoints_for_config(struct usb_hcd *hcd,
				struct oz_port *port);
static int oz_build_endpoints_for_interface(struct usb_hcd *hcd,
			struct oz_port *port,
			struct usb_host_interface *intf, gfp_t mem_flags);
static void oz_clean_endpoints_for_interface(struct usb_hcd *hcd,
			struct oz_port *port, int if_ix);
static void oz_process_ep0_urb(struct oz_hcd *ozhcd, struct urb *urb,
		gfp_t mem_flags);
static struct oz_urb_link *oz_remove_urb(struct oz_endpoint *ep,
		struct urb *urb);
static void oz_hcd_clear_orphanage(struct oz_hcd *ozhcd, int status);
/*------------------------------------------------------------------------------
 * Static external variables.
 */
static struct platform_device *g_plat_dev;
static struct oz_hcd *g_ozhcd;
static DEFINE_SPINLOCK(g_hcdlock);	/* Guards g_ozhcd. */
static const char g_hcd_name[] = "Ozmo WPAN";
static struct list_head *g_link_pool;
static int g_link_pool_size;
static DEFINE_SPINLOCK(g_link_lock);
static DEFINE_SPINLOCK(g_tasklet_lock);
static struct tasklet_struct g_urb_process_tasklet;
static struct tasklet_struct g_urb_cancel_tasklet;
static atomic_t g_pending_urbs = ATOMIC_INIT(0);
static atomic_t g_usb_frame_number = ATOMIC_INIT(0);
static const struct hc_driver g_oz_hc_drv = {
	.description =		g_hcd_name,
	.product_desc =		"Ozmo Devices WPAN",
	.hcd_priv_size =	sizeof(struct oz_hcd),
	.flags =		HCD_USB11,
	.start =		oz_hcd_start,
	.stop =			oz_hcd_stop,
	.shutdown =		oz_hcd_shutdown,
	.urb_enqueue =		oz_hcd_urb_enqueue,
	.urb_dequeue =		oz_hcd_urb_dequeue,
	.endpoint_disable =	oz_hcd_endpoint_disable,
	.endpoint_reset =	oz_hcd_endpoint_reset,
	.get_frame_number =	oz_hcd_get_frame_number,
	.hub_status_data =	oz_hcd_hub_status_data,
	.hub_control =		oz_hcd_hub_control,
	.bus_suspend =		oz_hcd_bus_suspend,
	.bus_resume =		oz_hcd_bus_resume,
};

static struct platform_driver g_oz_plat_drv = {
	.probe = oz_plat_probe,
	.remove = oz_plat_remove,
	.shutdown = oz_plat_shutdown,
	.suspend = oz_plat_suspend,
	.resume = oz_plat_resume,
	.driver = {
		.name = OZ_PLAT_DEV_NAME,
		.owner = THIS_MODULE,
	},
};
/*------------------------------------------------------------------------------
 * Gets our private context area (which is of type struct oz_hcd) from the
 * usb_hcd structure.
 * Context: any
 */
static inline struct oz_hcd *oz_hcd_private(struct usb_hcd *hcd)
{
	return (struct oz_hcd *)hcd->hcd_priv;
}
/*------------------------------------------------------------------------------
 * Searches list of ports to find the index of the one with a specified  USB
 * bus address. If none of the ports has the bus address then the connection
 * port is returned, if there is one or -1 otherwise.
 * Context: any
 */
static int oz_get_port_from_addr(struct oz_hcd *ozhcd, u8 bus_addr)
{
	int i;

	if (0 != bus_addr) {
		for (i = 0; i < OZ_NB_PORTS; i++) {
			if (ozhcd->ports[i].bus_addr == bus_addr)
				return i;
		}

		return -1;
	}
	return ozhcd->conn_port;
}
/*------------------------------------------------------------------------------
 * Allocates an urb link, first trying the pool but going to heap if empty.
 * Context: any
 */
static struct oz_urb_link *oz_alloc_urb_link(void)
{
	struct oz_urb_link *urbl = NULL;
	unsigned long irq_state;
	spin_lock_irqsave(&g_link_lock, irq_state);
	if (g_link_pool) {
		urbl = container_of(g_link_pool, struct oz_urb_link, link);
		g_link_pool = urbl->link.next;
		--g_link_pool_size;
	}
	spin_unlock_irqrestore(&g_link_lock, irq_state);
	if (urbl == NULL)
		urbl = kmalloc(sizeof(struct oz_urb_link), GFP_ATOMIC);
	return urbl;
}
/*------------------------------------------------------------------------------
 * Frees an urb link by putting it in the pool if there is enough space or
 * deallocating it to heap otherwise.
 * Context: any
 */
static void oz_free_urb_link(struct oz_urb_link *urbl)
{
	if (urbl) {
		unsigned long irq_state;
		spin_lock_irqsave(&g_link_lock, irq_state);
		if (g_link_pool_size < OZ_MAX_LINK_POOL_SIZE) {
			urbl->link.next = g_link_pool;
			g_link_pool = &urbl->link;
			urbl = NULL;
			g_link_pool_size++;
		}
		spin_unlock_irqrestore(&g_link_lock, irq_state);
		if (urbl)
			kfree(urbl);
	}
}
/*------------------------------------------------------------------------------
 * Deallocates all the urb links in the pool.
 * Context: unknown
 */
static void oz_empty_link_pool(void)
{
	struct list_head *e;
	unsigned long irq_state;
	spin_lock_irqsave(&g_link_lock, irq_state);
	e = g_link_pool;
	g_link_pool = NULL;
	g_link_pool_size = 0;
	spin_unlock_irqrestore(&g_link_lock, irq_state);
	while (e) {
		struct oz_urb_link *urbl =
			container_of(e, struct oz_urb_link, link);
		e = e->next;
		kfree(urbl);
	}
}
/*------------------------------------------------------------------------------
 * Allocates endpoint structure and optionally a buffer. If a buffer is
 * allocated it immediately follows the endpoint structure.
 * Context: softirq
 */
static struct oz_endpoint *oz_ep_alloc(gfp_t mem_flags, int buffer_size)
{
	struct oz_endpoint *ep =
		kzalloc(sizeof(struct oz_endpoint)+buffer_size, mem_flags);
	if (ep) {
		INIT_LIST_HEAD(&ep->urb_list);
		INIT_LIST_HEAD(&ep->link);
		ep->credit = -1;
		if (buffer_size) {
			ep->buffer_size = buffer_size;
			ep->buffer = (u8 *)(ep+1);
		}
	}
	return ep;
}
/*------------------------------------------------------------------------------
 * Pre-condition: Must be called with g_tasklet_lock held and interrupts
 * disabled.
 * Context: softirq or process
 */
struct oz_urb_link *oz_uncancel_urb(struct oz_hcd *ozhcd, struct urb *urb)
{
	struct oz_urb_link *urbl;
	struct list_head *e;
	list_for_each(e, &ozhcd->urb_cancel_list) {
		urbl = container_of(e, struct oz_urb_link, link);
		if (urb == urbl->urb) {
			list_del_init(e);
			return urbl;
		}
	}
	return NULL;
}
/*------------------------------------------------------------------------------
 * This is called when we have finished processing an urb. It unlinks it from
 * the ep and returns it to the core.
 * Context: softirq or process
 */
static void oz_complete_urb(struct usb_hcd *hcd, struct urb *urb,
		int status)
{
	struct oz_hcd *ozhcd = oz_hcd_private(hcd);
	unsigned long irq_state;
	struct oz_urb_link *cancel_urbl = NULL;
	spin_lock_irqsave(&g_tasklet_lock, irq_state);

	if (usb_pipeisoc(urb->pipe)) {
		if (status < 0) {
			int i;
			urb->transfer_buffer_length = 0;
			for (i = 0; i < urb->number_of_packets; i++) {
				urb->iso_frame_desc[i].actual_length = 0;
				urb->iso_frame_desc[i].status = status;
			}
		} else {
			/* ISOC checks transfer_buffer_length */
			urb->transfer_buffer_length = urb->actual_length;
		}
	}
	oz_trace_urb_out(urb, status);
	usb_hcd_unlink_urb_from_ep(hcd, urb);
	/* Clear hcpriv which will prevent it being put in the cancel list
	 * in the event that an attempt is made to cancel it.
	 */
	urb->hcpriv = NULL;
	/* Walk the cancel list in case the urb is already sitting there.
	 * Since we process the cancel list in a tasklet rather than in
	 * the dequeue function this could happen.
	 */
	cancel_urbl = oz_uncancel_urb(ozhcd, urb);
	/* Note: we release lock but do not enable local irqs.
	 * It appears that usb_hcd_giveback_urb() expects irqs to be disabled,
	 * or at least other host controllers disable interrupts at this point
	 * so we do the same. We must, however, release the lock otherwise a
	 * deadlock will occur if an urb is submitted to our driver in the urb
	 * completion function. Because we disable interrupts it is possible
	 * that the urb_enqueue function can be called with them disabled.
	 */
	spin_unlock(&g_tasklet_lock);
	if (oz_forget_urb(urb)) {
		oz_trace("OZWPAN: ERROR Unknown URB %p\n", urb);
	} else {
		atomic_dec(&g_pending_urbs);
		usb_hcd_giveback_urb(hcd, urb, status);
	}
	spin_lock(&g_tasklet_lock);
	spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
	if (cancel_urbl)
		oz_free_urb_link(cancel_urbl);
}
/*------------------------------------------------------------------------------
 * Deallocates an endpoint including deallocating any associated stream and
 * returning any queued urbs to the core.
 * Context: softirq
 */
static void oz_ep_free(struct oz_port *port, struct oz_endpoint *ep)
{
	if (port) {
		struct list_head list;
		struct oz_hcd *ozhcd = port->ozhcd;
		INIT_LIST_HEAD(&list);
		if (ep->flags & OZ_F_EP_HAVE_STREAM)
			oz_usb_stream_delete(port->hpd, ep->ep_num);
		/* Transfer URBs to the orphanage while we hold the lock. */
		spin_lock_bh(&ozhcd->hcd_lock);
		/* Note: this works even if ep->urb_list is empty.*/
		list_replace_init(&ep->urb_list, &list);
		/* Put the URBs in the orphanage. */
		list_splice_tail(&list, &ozhcd->orphanage);
		spin_unlock_bh(&ozhcd->hcd_lock);
	}
	kfree(ep);
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
void oz_complete_buffered_urb(struct oz_port *port, struct oz_endpoint *ep,
			struct urb *urb)
{
	int data_len, available_space, copy_len;

	data_len = ep->buffer[ep->out_ix];
	if (data_len <= urb->transfer_buffer_length)
		available_space = data_len;
	else
		available_space = urb->transfer_buffer_length;

	if (++ep->out_ix == ep->buffer_size)
		ep->out_ix = 0;
	copy_len = ep->buffer_size - ep->out_ix;
	if (copy_len >= available_space)
		copy_len = available_space;
	memcpy(urb->transfer_buffer, &ep->buffer[ep->out_ix], copy_len);

	if (copy_len < available_space) {
		memcpy((urb->transfer_buffer + copy_len), ep->buffer,
						(available_space - copy_len));
		ep->out_ix = available_space - copy_len;
	} else {
		ep->out_ix += copy_len;
	}
	urb->actual_length = available_space;
	if (ep->out_ix == ep->buffer_size)
		ep->out_ix = 0;

	ep->buffered_units--;
	oz_complete_urb(port->ozhcd->hcd, urb, 0);
}

/*------------------------------------------------------------------------------
 * Context: softirq
 */
static int oz_enqueue_ep_urb(struct oz_port *port, u8 ep_addr, int in_dir,
			struct urb *urb, u8 req_id)
{
	struct oz_urb_link *urbl;
	struct oz_endpoint *ep;
	int err = 0;
	if (ep_addr >= OZ_NB_ENDPOINTS) {
		oz_trace("Invalid endpoint number in oz_enqueue_ep_urb().\n");
		return -EINVAL;
	}
	urbl = oz_alloc_urb_link();
	if (!urbl)
		return -ENOMEM;
	urbl->submit_counter = 0;
	urbl->urb = urb;
	urbl->req_id = req_id;
	urbl->ep_num = ep_addr;
	/* Hold lock while we insert the URB into the list within the
	 * endpoint structure.
	 */
	spin_lock_bh(&port->ozhcd->hcd_lock);
	/* If the urb has been unlinked while out of any list then
	 * complete it now.
	 */
	if (urb->unlinked) {
		spin_unlock_bh(&port->ozhcd->hcd_lock);
		oz_trace("urb %p unlinked so complete immediately\n", urb);
		oz_complete_urb(port->ozhcd->hcd, urb, 0);
		oz_free_urb_link(urbl);
		return 0;
	}
	if (in_dir) {
		if (port->in_ep[ep_addr]) {
			ep = port->in_ep[ep_addr];
		} else {
			err = -EINVAL;
			goto out;
		}
	} else {
		if (port->out_ep[ep_addr]) {
			ep = port->out_ep[ep_addr];
		} else {
			err = -EINVAL;
			goto out;
		}
	}

	/*For interrupt endpoint check for buffered data
	* & complete urb
	*/
	if (((ep->attrib & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT)
						 && ep->buffered_units > 0) {
		oz_free_urb_link(urbl);
		spin_unlock_bh(&port->ozhcd->hcd_lock);
		oz_complete_buffered_urb(port, ep, urb);
		return 0;
	}

	if (ep && port->hpd) {
		list_add_tail(&urbl->link, &ep->urb_list);
		if (!in_dir && ep_addr && (ep->credit < 0)) {
			getrawmonotonic(&ep->timestamp);
			ep->credit = 0;
		}
	} else {
		err = -EPIPE;
	}
out:
	spin_unlock_bh(&port->ozhcd->hcd_lock);
	if (err)
		oz_free_urb_link(urbl);
	return err;
}
/*------------------------------------------------------------------------------
 * Removes an urb from the queue in the endpoint.
 * Returns 0 if it is found and -EIDRM otherwise.
 * Context: softirq
 */
static int oz_dequeue_ep_urb(struct oz_port *port, u8 ep_addr, int in_dir,
			struct urb *urb)
{
	struct oz_urb_link *urbl = NULL;
	struct oz_endpoint *ep;
	spin_lock_bh(&port->ozhcd->hcd_lock);
	if (in_dir)
		ep = port->in_ep[ep_addr];
	else
		ep = port->out_ep[ep_addr];
	if (ep) {
		struct list_head *e;
		list_for_each(e, &ep->urb_list) {
			urbl = container_of(e, struct oz_urb_link, link);
			if (urbl->urb == urb) {
				list_del_init(e);
				break;
			}
			urbl = NULL;
		}
	}
	spin_unlock_bh(&port->ozhcd->hcd_lock);
	if (urbl)
		oz_free_urb_link(urbl);
	return urbl ? 0 : -EIDRM;
}
/*------------------------------------------------------------------------------
 * Finds an urb given its request id.
 * Context: softirq
 */
static struct urb *oz_find_urb_by_id(struct oz_port *port, int ep_ix,
		u8 req_id)
{
	struct oz_hcd *ozhcd = port->ozhcd;
	struct urb *urb = NULL;
	struct oz_urb_link *urbl = NULL;
	struct oz_endpoint *ep;

	spin_lock_bh(&ozhcd->hcd_lock);
	ep = port->out_ep[ep_ix];
	if (ep) {
		struct list_head *e;
		list_for_each(e, &ep->urb_list) {
			urbl = container_of(e, struct oz_urb_link, link);
			if (urbl->req_id == req_id) {
				urb = urbl->urb;
				list_del_init(e);
				break;
			}
		}
	}
	spin_unlock_bh(&ozhcd->hcd_lock);
	/* If urb is non-zero then we we must have an urb link to delete.
	 */
	if (urb)
		oz_free_urb_link(urbl);
	return urb;
}
/*------------------------------------------------------------------------------
 * Finds an urbl given request it, then set submit_count to 1, thus heartbeat
 * count this value up to EP0_TIMEOUT.
 */
void oz_hcd_mark_urb_submitted(void *hport, int ep_ix, u8 req_id)
{
	struct oz_port *port = (struct oz_port *)hport;
	struct oz_hcd *ozhcd = port->ozhcd;
	struct oz_urb_link *urbl = 0;
	struct oz_endpoint *ep;
	unsigned long irq_state;
	unsigned found = 0;
	spin_lock_bh(&ozhcd->hcd_lock);
	spin_lock_irqsave(&g_tasklet_lock, irq_state);
	ep = port->out_ep[ep_ix];
	if (ep) {
		struct list_head *e;
		list_for_each(e, &ep->urb_list) {
			urbl = container_of(e, struct oz_urb_link, link);
			if (urbl->req_id == req_id) {
				urbl->submit_counter = 1;
				found = 1;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
	spin_unlock_bh(&ozhcd->hcd_lock);
}
/*------------------------------------------------------------------------------
 * Pre-condition: Port lock must be held.
 * Context: softirq
 */
static void oz_acquire_port(struct oz_port *port, void *hpd)
{
	INIT_LIST_HEAD(&port->isoc_out_ep);
	INIT_LIST_HEAD(&port->isoc_in_ep);
	port->flags |= OZ_PORT_F_PRESENT | OZ_PORT_F_CHANGED;
	port->status |= USB_PORT_STAT_CONNECTION |
			(USB_PORT_STAT_C_CONNECTION << 16);
	oz_usb_get(hpd);
	port->hpd = hpd;
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
static struct oz_hcd *oz_hcd_claim(void)
{
	struct oz_hcd *ozhcd;
	spin_lock_bh(&g_hcdlock);
	ozhcd = g_ozhcd;
	if (ozhcd)
		usb_get_hcd(ozhcd->hcd);
	spin_unlock_bh(&g_hcdlock);
	return ozhcd;
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
static inline void oz_hcd_put(struct oz_hcd *ozhcd)
{
	if (ozhcd)
		usb_put_hcd(ozhcd->hcd);
}
/*------------------------------------------------------------------------------
 * This is called by the protocol handler to notify that a PD has arrived.
 * We allocate a port to associate with the PD and create a structure for
 * endpoint 0. This port is made the connection port.
 * In the event that one of the other port is already a connection port then
 * we fail.
 * TODO We should be able to do better than fail and should be able remember
 * that this port needs configuring and make it the connection port once the
 * current connection port has been assigned an address. Collisions here are
 * probably very rare indeed.
 * Context: softirq
 */
void *oz_hcd_pd_arrived(void *hpd)
{
	int i;
	void *hport = NULL;
	struct oz_hcd *ozhcd = NULL;
	struct oz_endpoint *ep;
	static int n = OZ_NB_PORTS;
	int j;
	ozhcd = oz_hcd_claim();
	if (ozhcd == NULL)
		return NULL;
	/* Allocate an endpoint object in advance (before holding hcd lock) to
	 * use for out endpoint 0.
	 */
	ep = oz_ep_alloc(GFP_ATOMIC, 0);
	spin_lock_bh(&ozhcd->hcd_lock);
	if (ozhcd->conn_port >= 0) {
		spin_unlock_bh(&ozhcd->hcd_lock);
		goto out;
	}
	j = n;
	for (i = 0; i < OZ_NB_PORTS; i++) {
		struct oz_port *port;
		if (++j >= OZ_NB_PORTS)
			j = 0;
		port  = &ozhcd->ports[j];
		spin_lock(&port->port_lock);
		if ((port->flags & OZ_PORT_F_PRESENT) == 0) {
			oz_acquire_port(port, hpd);
			port->bus_addr = 0;
			port->config_num = 0;
			spin_unlock(&port->port_lock);
			break;
		}
		spin_unlock(&port->port_lock);
	}
	if (i < OZ_NB_PORTS) {
		ozhcd->conn_port = j;
		/* Attach out endpoint 0.
		 */
		ozhcd->ports[j].out_ep[0] = ep;
		ep = NULL;
		hport = &ozhcd->ports[j];
		spin_unlock_bh(&ozhcd->hcd_lock);
		if (ozhcd->flags & OZ_HDC_F_SUSPENDED) {
			oz_trace_msg(H, "usb_hcd_resume_root_hub()\n");
			usb_hcd_resume_root_hub(ozhcd->hcd);
		}
		n = j;
		oz_trace_msg(H, "usb_hcd_poll_rh()\n");
		usb_hcd_poll_rh_status(ozhcd->hcd);
	} else {
		spin_unlock_bh(&ozhcd->hcd_lock);
	}
out:
	if (ep) /* ep is non-null if not used. */
		oz_ep_free(NULL, ep);
	oz_hcd_put(ozhcd);
	return hport;
}
/*------------------------------------------------------------------------------
 * This is called by the protocol handler to notify that the PD has gone away.
 * We need to deallocate all resources and then request that the root hub is
 * polled. We release the reference we hold on the PD.
 * Context: softirq
 */
void oz_hcd_pd_departed(void *hport)
{
	struct oz_port *port = (struct oz_port *)hport;
	struct oz_hcd *ozhcd;
	void *hpd;
	struct usb_hcd *hcd;
	struct oz_endpoint *ep = NULL;

	oz_trace("%s:\n", __func__);
	if (port == NULL) {
		oz_trace("%s: port = 0\n", __func__);
		return;
	}
	ozhcd = port->ozhcd;
	if (ozhcd == NULL)
		return;
	/* Check if this is the connection port - if so clear it.
	 */
	spin_lock_bh(&ozhcd->hcd_lock);
	if ((ozhcd->conn_port >= 0) &&
		(port == &ozhcd->ports[ozhcd->conn_port])) {
		ozhcd->conn_port = -1;
	}
	spin_lock(&port->port_lock);
	port->flags |= OZ_PORT_F_DYING;
	spin_unlock(&port->port_lock);
	spin_unlock_bh(&ozhcd->hcd_lock);

	oz_clean_endpoints_for_config(ozhcd->hcd, port);
	spin_lock_bh(&port->port_lock);
	hpd = port->hpd;
	port->hpd = NULL;
	port->bus_addr = 0xff;
	port->config_num = 0;
	port->flags &= ~(OZ_PORT_F_PRESENT | OZ_PORT_F_DYING);
	port->flags |= OZ_PORT_F_CHANGED;
	port->status &= ~(USB_PORT_STAT_CONNECTION|USB_PORT_STAT_ENABLE);
	port->status |= (USB_PORT_STAT_C_CONNECTION << 16);
	/* If there is an endpont 0 then clear the pointer while we hold
	 * the spinlock be we deallocate it after releasing the lock.
	 */
	if (port->out_ep[0]) {
		ep = port->out_ep[0];
		port->out_ep[0] = NULL;
	}
	spin_unlock_bh(&port->port_lock);
	if (ep)
		oz_ep_free(port, ep);
	hcd = ozhcd->hcd;
	clear_bit(HCD_FLAG_POLL_RH, &hcd->flags);
	oz_trace_msg(H, "usb_hcd_poll_rh_status()\n");
	usb_hcd_poll_rh_status(ozhcd->hcd);
	oz_usb_put(hpd);
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
void oz_hcd_pd_reset(void *hpd, void *hport)
{
	/* Cleanup the current configuration and report reset to the core.
	 */
	struct oz_port *port = (struct oz_port *)hport;
	struct oz_hcd *ozhcd = port->ozhcd;
	oz_trace("PD Reset\n");
	spin_lock_bh(&port->port_lock);
	port->flags |= OZ_PORT_F_CHANGED;
	port->status |= USB_PORT_STAT_RESET;
	port->status |= (USB_PORT_STAT_C_RESET << 16);
	spin_unlock_bh(&port->port_lock);
	oz_clean_endpoints_for_config(ozhcd->hcd, port);
	usb_hcd_poll_rh_status(ozhcd->hcd);
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
void oz_hcd_get_desc_cnf(void *hport, u8 req_id, int status, const u8 *desc,
			int length, int offset, int total_size)
{
	struct oz_port *port = (struct oz_port *)hport;
	struct urb *urb;
	int err = 0;

	urb = oz_find_urb_by_id(port, 0, req_id);
	if (!urb)
		return;
	if (status == 0) {
		int copy_len;
		int required_size = urb->transfer_buffer_length;
		if (required_size > total_size)
			required_size = total_size;
		copy_len = required_size-offset;
		if (length <= copy_len)
			copy_len = length;
		memcpy(urb->transfer_buffer+offset, desc, copy_len);
		offset += copy_len;
		if (offset < required_size) {
			struct usb_ctrlrequest *setup =
				(struct usb_ctrlrequest *)urb->setup_packet;
			unsigned wvalue = le16_to_cpu(setup->wValue);
			if (oz_enqueue_ep_urb(port, 0, 0, urb, req_id))
				err = -ENOMEM;
			else if (oz_usb_get_desc_req(port->hpd, req_id,
					setup->bRequestType, (u8)(wvalue>>8),
					(u8)wvalue, setup->wIndex, offset,
					required_size-offset)) {
				oz_dequeue_ep_urb(port, 0, 0, urb);
				err = -ENOMEM;
			}
			if (err == 0)
				return;
		}
	}
	urb->actual_length = total_size;
	oz_complete_urb(port->ozhcd->hcd, urb, 0);
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
static void oz_hcd_complete_set_config(struct oz_port *port, struct urb *urb,
		u8 rcode, u8 config_num)
{
	int rc = 0;
	struct usb_hcd *hcd = port->ozhcd->hcd;
	if (rcode == 0) {
		port->config_num = config_num;
		oz_clean_endpoints_for_config(hcd, port);
		if (oz_build_endpoints_for_config(hcd, port,
			&urb->dev->config[port->config_num-1], GFP_ATOMIC)) {
			rc = -ENOMEM;
		}
	} else {
		rc = -ENOMEM;
	}
	oz_complete_urb(hcd, urb, rc);
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
static void oz_hcd_complete_set_interface(struct oz_port *port, struct urb *urb,
		u8 rcode, u8 if_num, u8 alt)
{
	struct usb_hcd *hcd = port->ozhcd->hcd;
	int rc = 0;
	if (rcode == 0) {
		if (port->config_num > 0) {
			struct usb_host_config *config;
			struct usb_host_interface *intf;

			oz_clean_endpoints_for_interface(hcd, port, if_num);
			config = &urb->dev->config[port->config_num-1];
			intf = &config->intf_cache[if_num]->altsetting[alt];
			if (oz_build_endpoints_for_interface(hcd, port, intf,
				GFP_ATOMIC))
				rc = -ENOMEM;
			else
				port->iface[if_num].alt = alt;
		} else
			rc = -ENOMEM;
	} else {
		rc = -ENOMEM;
	}
	oz_complete_urb(hcd, urb, rc);
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
void oz_hcd_control_cnf(void *hport, u8 req_id, u8 rcode, const u8 *data,
	int data_len)
{
	struct oz_port *port = (struct oz_port *)hport;
	struct urb *urb;
	struct usb_ctrlrequest *setup;
	struct usb_hcd *hcd = port->ozhcd->hcd;
	unsigned windex;
	unsigned wvalue;

	urb = oz_find_urb_by_id(port, 0, req_id);
	if (!urb) {
		oz_trace("URB not found: %p\n", urb);
		return;
	}
	setup = (struct usb_ctrlrequest *)urb->setup_packet;
	windex = le16_to_cpu(setup->wIndex);
	wvalue = le16_to_cpu(setup->wValue);
	if ((setup->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
		/* Standard requests */
		switch (setup->bRequest) {
		case USB_REQ_SET_CONFIGURATION:
			oz_hcd_complete_set_config(port, urb, rcode,
				(u8)wvalue);
			break;
		case USB_REQ_SET_INTERFACE:
			oz_hcd_complete_set_interface(port, urb, rcode,
				(u8)windex, (u8)wvalue);
			break;
		default:
			oz_complete_urb(hcd, urb, 0);
		}

	} else {
		int copy_len;
		if (data_len) {
			if (data_len <= urb->transfer_buffer_length)
				copy_len = data_len;
			else
				copy_len = urb->transfer_buffer_length;
			memcpy(urb->transfer_buffer, data, copy_len);
			urb->actual_length = copy_len;
		}
		oz_complete_urb(hcd, urb, 0);
	}
}
/*------------------------------------------------------------------------------
 * Context: softirq-serialized
 */
static int oz_hcd_buffer_data(struct oz_endpoint *ep, const u8 *data,
				int data_len)
{
	int space;
	int copy_len;
	if (!ep->buffer)
		return -1;
	space = ep->out_ix-ep->in_ix-1;
	if (space < 0)
		space += ep->buffer_size;
	if (space < (data_len+1)) {
		oz_trace_msg(I, "EP:%02X u:%d FULL len:%d spc:%d\n",
				ep->ep_num | USB_DIR_IN,
				ep->buffered_units,
				data_len, space);
		return -1;
	}
	ep->buffer[ep->in_ix] = (u8)data_len;
	if (++ep->in_ix == ep->buffer_size)
		ep->in_ix = 0;
	copy_len = ep->buffer_size - ep->in_ix;
	if (copy_len > data_len)
		copy_len = data_len;
	memcpy(&ep->buffer[ep->in_ix], data, copy_len);

	if (copy_len < data_len) {
		memcpy(ep->buffer, data+copy_len, data_len-copy_len);
		ep->in_ix = data_len-copy_len;
	} else {
		ep->in_ix += copy_len;
	}
	if (ep->in_ix == ep->buffer_size)
		ep->in_ix = 0;
	ep->buffered_units++;
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: softirq-serialized
 */
void oz_hcd_data_ind(void *hport, u8 endpoint, const u8 *data, int data_len)
{
	struct oz_port *port = (struct oz_port *)hport;
	struct oz_endpoint *ep;
	struct oz_hcd *ozhcd = port->ozhcd;
	spin_lock_bh(&ozhcd->hcd_lock);
	ep = port->in_ep[endpoint & USB_ENDPOINT_NUMBER_MASK];
	if (ep == NULL)
		goto done;
	switch (ep->attrib & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_INT:
	case USB_ENDPOINT_XFER_BULK:
		if (!list_empty(&ep->urb_list)) {
			struct oz_urb_link *urbl =
				list_first_entry(&ep->urb_list,
					struct oz_urb_link, link);
			struct urb *urb;
			int copy_len;
			list_del_init(&urbl->link);
			spin_unlock_bh(&ozhcd->hcd_lock);
			urb = urbl->urb;
			oz_free_urb_link(urbl);
			if (data_len <= urb->transfer_buffer_length)
				copy_len = data_len;
			else
				copy_len = urb->transfer_buffer_length;
			memcpy(urb->transfer_buffer, data, copy_len);
			urb->actual_length = copy_len;
			oz_complete_urb(port->ozhcd->hcd, urb, 0);
			return;
		} else {
			oz_hcd_buffer_data(ep, data, data_len);
		}
		break;
	case USB_ENDPOINT_XFER_ISOC:
		if (oz_hcd_buffer_data(ep, data, data_len) != 0) {
			int len;
			int copy_len;
			while (ep->buffered_units > ep->max_buffer_units) {
				len = ep->buffer[ep->out_ix];
				if (++ep->out_ix == ep->buffer_size)
					ep->out_ix = 0;
				copy_len = ep->buffer_size - ep->out_ix;
				if (copy_len > len)
					copy_len = len;
				if (copy_len < len)
					ep->out_ix = len - copy_len;
				else
					ep->out_ix += copy_len;

				if (ep->out_ix == ep->buffer_size)
					ep->out_ix = 0;

				ep->buffered_units--;
			}
			ep->flags |= OZ_F_EP_BUFFERING;
		}
		break;
	}
done:
	spin_unlock_bh(&ozhcd->hcd_lock);
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static inline int oz_usb_get_frame_number(void)
{
	return atomic_inc_return(&g_usb_frame_number);
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
int oz_hcd_heartbeat(void *hport)
{
	int rc = 0;
	struct oz_port *port = (struct oz_port *)hport;
	struct oz_hcd *ozhcd = port->ozhcd;
	struct oz_urb_link *urbl;
	struct list_head xfr_list;
	struct list_head *e;
	struct list_head *n;
	struct urb *urb;
	struct oz_endpoint *ep;
	struct timespec ts, delta;
	getrawmonotonic(&ts);
	INIT_LIST_HEAD(&xfr_list);
	/* Check the OUT isoc endpoints to see if any URB data can be sent.
	 */
	spin_lock_bh(&ozhcd->hcd_lock);
	list_for_each(e, &port->isoc_out_ep) {
		ep = ep_from_link(e);
		if (ep->credit < 0)
			continue;
		delta = timespec_sub(ts, ep->timestamp);
		ep->credit += div64_u64(timespec_to_ns(&delta), NSEC_PER_MSEC);
		if (ep->credit > ep->credit_ceiling)
			ep->credit = ep->credit_ceiling;
		ep->timestamp = ts;
		while (ep->credit && !list_empty(&ep->urb_list)) {
			urbl = list_first_entry(&ep->urb_list,
				struct oz_urb_link, link);
			urb = urbl->urb;
			if ((ep->credit + 1) < urb->number_of_packets)
				break;
			ep->credit -= urb->number_of_packets;
			if (ep->credit < 0)
				ep->credit = 0;
			list_move_tail(&urbl->link, &xfr_list);
		}
	}
	spin_unlock_bh(&ozhcd->hcd_lock);
	/* Send to PD and complete URBs.
	 */
	list_for_each_safe(e, n, &xfr_list) {
		urbl = container_of(e, struct oz_urb_link, link);
		urb = urbl->urb;
		list_del_init(e);
		urb->error_count = 0;
		urb->start_frame = oz_usb_get_frame_number();
		oz_usb_send_isoc(port->hpd, urbl->ep_num, urb);
		oz_free_urb_link(urbl);
		oz_complete_urb(port->ozhcd->hcd, urb, 0);
	}
	/* Check the IN isoc endpoints to see if any URBs can be completed.
	 */
	spin_lock_bh(&ozhcd->hcd_lock);
	list_for_each(e, &port->isoc_in_ep) {
		struct oz_endpoint *ep = ep_from_link(e);

		if (ep->flags & OZ_F_EP_BUFFERING) {
			if (ep->buffered_units >= ep->max_buffer_units) {
				ep->flags &= ~OZ_F_EP_BUFFERING;
				ep->credit = 0;
				ep->credit2 = 0;
				ep->timestamp = ts;
				ep->start_frame = 0;
			}
			continue;
		}
		delta = timespec_sub(ts, ep->timestamp);
		ep->credit += div64_u64(timespec_to_ns(&delta)+5000, NSEC_PER_MSEC);
		ep->timestamp = ts;
		while (!list_empty(&ep->urb_list)) {
			struct oz_urb_link *urbl =
				list_first_entry(&ep->urb_list,
					struct oz_urb_link, link);
			struct urb *urb = urbl->urb;
			int len = 0;
			int copy_len;
			int i;
			if (ep->buffered_units < urb->number_of_packets)
				break;
			urb->actual_length = 0;
			for (i = 0; i < urb->number_of_packets; i++) {
				len = ep->buffer[ep->out_ix];
				if (++ep->out_ix == ep->buffer_size)
					ep->out_ix = 0;
				copy_len = ep->buffer_size - ep->out_ix;
				if (copy_len > len)
					copy_len = len;
				memcpy(urb->transfer_buffer,
					&ep->buffer[ep->out_ix], copy_len);
				if (copy_len < len) {
					memcpy(urb->transfer_buffer+copy_len,
						ep->buffer, len-copy_len);
					ep->out_ix = len-copy_len;
				} else
					ep->out_ix += copy_len;
				if (ep->out_ix == ep->buffer_size)
					ep->out_ix = 0;
				urb->iso_frame_desc[i].offset =
					urb->actual_length;
				urb->actual_length += len;
				urb->iso_frame_desc[i].actual_length = len;
				urb->iso_frame_desc[i].status = 0;
			}
			ep->buffered_units -= urb->number_of_packets;
			urb->error_count = 0;
			urb->start_frame = ep->start_frame;
			ep->start_frame += urb->number_of_packets;
			list_move_tail(&urbl->link, &xfr_list);
			ep->credit -= urb->number_of_packets;
			ep->credit2 += urb->number_of_packets;
		}
		if (ep->buffered_units == 0) {
			oz_trace_msg(I, "EP:%02X Buffer under run\n",
					ep->ep_num | USB_DIR_IN);
			ep->flags |= OZ_F_EP_BUFFERING;
			continue;
		}
		if (ep->credit2 >= 1000)
		{
			static int buffered_units=-1;
			static int max_buffer_units=-1;
			{
				int diff = ep->buffered_units - buffered_units;
				oz_trace_msg(I, "u:%d o:%04d b:%d\n",
						ep->credit2,
						ep->credit2 + diff,
						ep->buffered_units);

				buffered_units = ep->buffered_units;
				max_buffer_units = ep->max_buffer_units;
			}
			ep->credit2 = 0;
		}
	}
	if (!list_empty(&port->isoc_out_ep) || !list_empty(&port->isoc_in_ep))
		rc = 1;
	spin_unlock_bh(&ozhcd->hcd_lock);
	/* Complete the filled URBs.
	 */
	list_for_each_safe(e, n, &xfr_list) {
		urbl = container_of(e, struct oz_urb_link, link);
		urb = urbl->urb;
		list_del_init(e);
		oz_free_urb_link(urbl);
		oz_complete_urb(port->ozhcd->hcd, urb, 0);
	}
	/* Check if there are any ep0 requests that have timed out.
	 * If so resent to PD.
	 */
	ep = port->out_ep[0];
	if (ep) {
		struct list_head *e;
		struct list_head *n;
		spin_lock_bh(&ozhcd->hcd_lock);
		list_for_each_safe(e, n, &ep->urb_list) {
			urbl = container_of(e, struct oz_urb_link, link);
			if (urbl->submit_counter > EP0_TIMEOUT_COUNTER) {
				oz_trace_msg(M, "URB:%08X timeout %02X\n",
					(unsigned int)((uintptr_t)urbl->urb),
					urbl->req_id);
				list_move_tail(e, &xfr_list);
				urbl->submit_counter = 0;
			} else if (urbl->submit_counter) {
				urbl->submit_counter++;
			}
		}
		if (!list_empty(&ep->urb_list))
			rc = 1;
		spin_unlock_bh(&ozhcd->hcd_lock);
		e = xfr_list.next;
		while (e != &xfr_list) {
			urbl = container_of(e, struct oz_urb_link, link);
			e = e->next;
			oz_trace_msg(M, "Resending request to PD.\n");
			oz_process_ep0_urb(ozhcd, urbl->urb, GFP_ATOMIC);
			oz_free_urb_link(urbl);
		}
	}
	return rc;
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
static int oz_build_endpoints_for_interface(struct usb_hcd *hcd,
		struct oz_port *port,
		struct usb_host_interface *intf, gfp_t mem_flags)
{
	struct oz_hcd *ozhcd = port->ozhcd;
	int i;
	int if_ix;
	int request_heartbeat = 0;

	if (intf == NULL)
		return -ENOMEM;

	if_ix = intf->desc.bInterfaceNumber;
	oz_trace("interface[%d] = %p\n", if_ix, intf);
	if (if_ix >= port->num_iface || port->iface == NULL)
		return -ENOMEM;

	for (i = 0; i < intf->desc.bNumEndpoints; i++) {
		struct usb_host_endpoint *hep = &intf->endpoint[i];
		u8 ep_addr = hep->desc.bEndpointAddress;
		u8 ep_num = ep_addr & USB_ENDPOINT_NUMBER_MASK;
		struct oz_endpoint *ep;
		int buffer_size = 0;

		if (ep_addr & USB_ENDPOINT_DIR_MASK) {
			switch (hep->desc.bmAttributes &
						USB_ENDPOINT_XFERTYPE_MASK) {
			case USB_ENDPOINT_XFER_ISOC:
				buffer_size = OZ_EP_BUFFER_SIZE_ISOC;
				break;
			case USB_ENDPOINT_XFER_INT:
				buffer_size = OZ_EP_BUFFER_SIZE_INT;
				break;
			}
		}

		ep = oz_ep_alloc(mem_flags, buffer_size);
		if (!ep) {
			oz_clean_endpoints_for_interface(hcd, port, if_ix);
			return -ENOMEM;
		}
		ep->attrib = hep->desc.bmAttributes;
		ep->ep_num = ep_num;
		if ((ep->attrib & USB_ENDPOINT_XFERTYPE_MASK)
			== USB_ENDPOINT_XFER_ISOC) {
			ep->credit_ceiling = 200;
			if (ep_addr & USB_ENDPOINT_DIR_MASK) {
				ep->max_buffer_units =
					 oz_get_up_max_buffer_units(port->hpd);
				ep->flags |= OZ_F_EP_BUFFERING;
			} else {
				ep->flags |= OZ_F_EP_HAVE_STREAM;
				if (oz_usb_stream_create(port->hpd, ep_num))
					ep->flags &= ~OZ_F_EP_HAVE_STREAM;
			}
		}
		spin_lock_bh(&ozhcd->hcd_lock);
		if (ep_addr & USB_ENDPOINT_DIR_MASK) {
			port->in_ep[ep_num] = ep;
			port->iface[if_ix].ep_mask |=
				(1<<(ep_num+OZ_NB_ENDPOINTS));
			if ((ep->attrib & USB_ENDPOINT_XFERTYPE_MASK)
				 == USB_ENDPOINT_XFER_ISOC) {
				list_add_tail(&ep->link, &port->isoc_in_ep);
				request_heartbeat = 1;
			}
		} else {
			port->out_ep[ep_num] = ep;
			port->iface[if_ix].ep_mask |= (1<<ep_num);
			if ((ep->attrib & USB_ENDPOINT_XFERTYPE_MASK)
				== USB_ENDPOINT_XFER_ISOC) {
				list_add_tail(&ep->link, &port->isoc_out_ep);
				request_heartbeat = 1;
			}
		}
		spin_unlock_bh(&ozhcd->hcd_lock);
		if (request_heartbeat && port->hpd)
			oz_usb_request_heartbeat(port->hpd);
	}
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
static void oz_clean_endpoints_for_interface(struct usb_hcd *hcd,
			struct oz_port *port, int if_ix)
{
	struct oz_hcd *ozhcd = port->ozhcd;
	unsigned mask;
	int i;
	struct list_head ep_list;

	oz_trace("Deleting endpoints for interface %d\n", if_ix);
	if (if_ix >= port->num_iface)
		return;
	INIT_LIST_HEAD(&ep_list);
	spin_lock_bh(&ozhcd->hcd_lock);
	mask = port->iface[if_ix].ep_mask;
	port->iface[if_ix].ep_mask = 0;
	for (i = 0; i < OZ_NB_ENDPOINTS; i++) {
		struct list_head *e;
		/* Gather OUT endpoints.
		 */
		if ((mask & (1<<i)) && port->out_ep[i]) {
			e = &port->out_ep[i]->link;
			port->out_ep[i] = NULL;
			/* Remove from isoc list if present.
			 */
			list_move_tail(e, &ep_list);
		}
		/* Gather IN endpoints.
		 */
		if ((mask & (1<<(i+OZ_NB_ENDPOINTS))) && port->in_ep[i]) {
			e = &port->in_ep[i]->link;
			port->in_ep[i] = NULL;
			list_move_tail(e, &ep_list);
		}
	}
	spin_unlock_bh(&ozhcd->hcd_lock);
	while (!list_empty(&ep_list)) {
		struct oz_endpoint *ep =
			list_first_entry(&ep_list, struct oz_endpoint, link);
		list_del_init(&ep->link);
		oz_ep_free(port, ep);
	}
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
static int oz_build_endpoints_for_config(struct usb_hcd *hcd,
		struct oz_port *port, struct usb_host_config *config,
		gfp_t mem_flags)
{
	struct oz_hcd *ozhcd = port->ozhcd;
	int i;
	int num_iface = config->desc.bNumInterfaces;
	if (num_iface) {
		struct oz_interface *iface;

		iface = kmalloc(num_iface*sizeof(struct oz_interface),
				mem_flags | __GFP_ZERO);
		if (!iface)
			return -ENOMEM;
		spin_lock_bh(&ozhcd->hcd_lock);
		port->iface = iface;
		port->num_iface = num_iface;
		spin_unlock_bh(&ozhcd->hcd_lock);
	}
	for (i = 0; i < num_iface; i++) {
		struct usb_host_interface *intf =
			&config->intf_cache[i]->altsetting[0];
		if (oz_build_endpoints_for_interface(hcd, port, intf,
			mem_flags))
			goto fail;
	}
	return 0;
fail:
	oz_clean_endpoints_for_config(hcd, port);
	return -1;
}
/*------------------------------------------------------------------------------
 * Context: softirq
 */
static void oz_clean_endpoints_for_config(struct usb_hcd *hcd,
			struct oz_port *port)
{
	struct oz_hcd *ozhcd = port->ozhcd;
	int i;
	oz_trace("Deleting endpoints for configuration.\n");
	for (i = 0; i < port->num_iface; i++)
		oz_clean_endpoints_for_interface(hcd, port, i);
	spin_lock_bh(&ozhcd->hcd_lock);
	if (port->iface) {
		kfree(port->iface);
		port->iface = NULL;
	}
	port->num_iface = 0;
	spin_unlock_bh(&ozhcd->hcd_lock);
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static void *oz_claim_hpd(struct oz_port *port)
{
	void *hpd = NULL;
	struct oz_hcd *ozhcd = port->ozhcd;
	spin_lock_bh(&ozhcd->hcd_lock);
	hpd = port->hpd;
	if (hpd)
		oz_usb_get(hpd);
	spin_unlock_bh(&ozhcd->hcd_lock);
	return hpd;
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static void oz_process_ep0_urb(struct oz_hcd *ozhcd, struct urb *urb,
		gfp_t mem_flags)
{
	struct usb_ctrlrequest *setup;
	unsigned windex;
	unsigned wvalue;
	unsigned wlength;
	void *hpd = NULL;
	u8 req_id;
	int rc = 0;
	unsigned complete = 0;

	int port_ix = -1;
	struct oz_port *port = NULL;

	port_ix = oz_get_port_from_addr(ozhcd, (usb_pipedevice(urb->pipe)));
	if (port_ix < 0) {
		rc = -EPIPE;
		goto out;
	}
	port =  &ozhcd->ports[port_ix];
	if (((port->flags & OZ_PORT_F_PRESENT) == 0)
		|| (port->flags & OZ_PORT_F_DYING)) {
		oz_trace("Refusing URB port_ix = %d devnum = %d\n",
			port_ix, urb->dev->devnum);
		rc = -EPIPE;
		goto out;
	}
	/* Store port in private context data.
	 */
	urb->hcpriv = port;
	setup = (struct usb_ctrlrequest *)urb->setup_packet;
	windex = le16_to_cpu(setup->wIndex);
	wvalue = le16_to_cpu(setup->wValue);
	wlength = le16_to_cpu(setup->wLength);

	req_id = port->next_req_id++;
	hpd = oz_claim_hpd(port);
	if (hpd == NULL) {
		rc = -EPIPE;
		goto out;
	}

	if ((setup->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
		/* Standard requests
		 */
		switch (setup->bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			break;
		case USB_REQ_SET_ADDRESS:
			spin_lock_bh(&ozhcd->hcd_lock);
			if (ozhcd->conn_port >= 0) {
				ozhcd->ports[ozhcd->conn_port].bus_addr =
					(u8)le16_to_cpu(setup->wValue);
				ozhcd->conn_port = -1;
			}
			spin_unlock_bh(&ozhcd->hcd_lock);
			complete = 1;
			break;
		case USB_REQ_SET_CONFIGURATION:
			break;
		case USB_REQ_GET_CONFIGURATION:
			/* We short circuit this case and reply directly since
			 * we have the selected configuration number cached.
			 */
			if (urb->transfer_buffer_length >= 1) {
				urb->actual_length = 1;
				*((u8 *)urb->transfer_buffer) =
					port->config_num;
				complete = 1;
			} else {
				rc = -EPIPE;
			}
			break;
		case USB_REQ_GET_INTERFACE:
			/* We short circuit this case and reply directly since
			 * we have the selected interface alternative cached.
			 */
			if (urb->transfer_buffer_length >= 1) {
				urb->actual_length = 1;
				*((u8 *)urb->transfer_buffer) =
					port->iface[(u8)windex].alt;
				complete = 1;
			} else {
				rc = -EPIPE;
			}
			break;
		case USB_REQ_SET_INTERFACE:
			break;
		}
	}
	if (!rc && !complete) {
		int data_len = 0;
		if ((setup->bRequestType & USB_DIR_IN) == 0)
			data_len = wlength;
		urb->actual_length = data_len;
		if (oz_usb_control_req(port->hpd, req_id, setup,
				urb->transfer_buffer, data_len)) {
			rc = -ENOMEM;
		} else {
			/* Note: we are queuing the request after we have
			 * submitted it to be transmitted. If the request were
			 * to complete before we queued it then it would not
			 * be found in the queue. It seems impossible for
			 * this to happen but if it did the request would
			 * be resubmitted so the problem would hopefully
			 * resolve itself. Putting the request into the
			 * queue before it has been sent is worse since the
			 * urb could be cancelled while we are using it
			 * to build the request.
			 */
			if (oz_enqueue_ep_urb(port, 0, 0, urb, req_id))
				rc = -ENOMEM;
		}
	}
	oz_usb_put(hpd);
out:
	if (rc || complete) {
		oz_complete_urb(ozhcd->hcd, urb, rc);
	} else {
		oz_usb_request_heartbeat(port->hpd);
	}
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static int oz_urb_process(struct oz_hcd *ozhcd, struct urb *urb)
{
	int rc = 0;
	struct oz_port *port = urb->hcpriv;
	u8 ep_addr;
	/* When we are paranoid we keep a list of urbs which we check against
	 * before handing one back. This is just for debugging during
	 * development and should be turned off in the released driver.
	 */
	oz_remember_urb(urb);
	/* Check buffer is valid.
	 */
	if (!urb->transfer_buffer && urb->transfer_buffer_length)
		return -EINVAL;
	/* Check if there is a device at the port - refuse if not.
	 */
	if ((port->flags & OZ_PORT_F_PRESENT) == 0)
		return -ENODEV;
	ep_addr = usb_pipeendpoint(urb->pipe);
	if (ep_addr) {
		/* If the request is not for EP0 then queue it.
		 */
		if (oz_enqueue_ep_urb(port, ep_addr, usb_pipein(urb->pipe),
			urb, 0))
			rc = -ENOENT;
	} else {
		oz_process_ep0_urb(ozhcd, urb, GFP_ATOMIC);
	}
	return rc;
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static void oz_urb_process_tasklet(unsigned long unused)
{
	unsigned long irq_state;
	struct urb *urb;
	struct oz_hcd *ozhcd = oz_hcd_claim();
	int rc = 0;
	if (ozhcd == NULL)
		return;
	/* This is called from a tasklet so is in softirq context but the urb
	 * list is filled from any context so we need to lock
	 * appropriately while removing urbs.
	 */
	spin_lock_irqsave(&g_tasklet_lock, irq_state);
	while (!list_empty(&ozhcd->urb_pending_list)) {
		struct oz_urb_link *urbl =
			list_first_entry(&ozhcd->urb_pending_list,
				struct oz_urb_link, link);
		list_del_init(&urbl->link);
		spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
		urb = urbl->urb;
		oz_free_urb_link(urbl);
		rc = oz_urb_process(ozhcd, urb);
		if (rc)
			oz_complete_urb(ozhcd->hcd, urb, rc);
		spin_lock_irqsave(&g_tasklet_lock, irq_state);
	}
	spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
	oz_hcd_put(ozhcd);
}
/*------------------------------------------------------------------------------
 * This function searches for the urb in any of the lists it could be in.
 * If it is found it is removed from the list and completed. If the urb is
 * being processed then it won't be in a list so won't be found. However, the
 * call to usb_hcd_check_unlink_urb() will set the value of the unlinked field
 * to a non-zero value. When an attempt is made to put the urb back in a list
 * the unlinked field will be checked and the urb will then be completed.
 * Context: tasklet
 */
static void oz_urb_cancel(struct oz_port *port, u8 ep_num, struct urb *urb)
{
	struct oz_urb_link *urbl = NULL;
	struct list_head *e;
	struct oz_hcd *ozhcd;
	unsigned long irq_state;
	u8 ix;
	if (port == NULL) {
		oz_trace("%s: urb=%p port is null\n", __func__, urb);
		return;
	}
	ozhcd = port->ozhcd;
	if (ozhcd == NULL) {
		oz_trace("%s: urb=%p ozhcd is null\n", __func__, urb);
		return;
	}

	/* Look in the tasklet queue.
	 */
	spin_lock_irqsave(&g_tasklet_lock, irq_state);
	list_for_each(e, &ozhcd->urb_cancel_list) {
		urbl = container_of(e, struct oz_urb_link, link);
		if (urb == urbl->urb) {
			list_del_init(e);
			spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
			goto out2;
		}
	}
	spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
	urbl = NULL;

	/* Look in the orphanage.
	 */
	spin_lock_irqsave(&ozhcd->hcd_lock, irq_state);
	list_for_each(e, &ozhcd->orphanage) {
		urbl = container_of(e, struct oz_urb_link, link);
		if (urbl->urb == urb) {
			list_del(e);
			oz_trace("Found urb in orphanage urb=%p\n", urb);
			goto out;
		}
	}
	ix = (ep_num & 0xf);
	urbl = NULL;
	if ((ep_num & USB_DIR_IN) && ix)
		urbl = oz_remove_urb(port->in_ep[ix], urb);
	else
		urbl = oz_remove_urb(port->out_ep[ix], urb);
out:
	spin_unlock_irqrestore(&ozhcd->hcd_lock, irq_state);
out2:
	if (urbl) {
		urb->actual_length = 0;
		oz_free_urb_link(urbl);
		oz_complete_urb(ozhcd->hcd, urb, -ENOENT);
	}
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static void oz_urb_cancel_tasklet(unsigned long unused)
{
	unsigned long irq_state;
	struct urb *urb;
	struct oz_hcd *ozhcd = oz_hcd_claim();
	if (ozhcd == NULL)
		return;
	spin_lock_irqsave(&g_tasklet_lock, irq_state);
	while (!list_empty(&ozhcd->urb_cancel_list)) {
		struct oz_urb_link *urbl =
			list_first_entry(&ozhcd->urb_cancel_list,
				struct oz_urb_link, link);
		list_del_init(&urbl->link);
		spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
		urb = urbl->urb;
		if (urb->unlinked)
			oz_urb_cancel(urbl->port, urbl->ep_num, urb);
		oz_free_urb_link(urbl);
		spin_lock_irqsave(&g_tasklet_lock, irq_state);
	}
	spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
	oz_hcd_put(ozhcd);
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static void oz_hcd_clear_orphanage(struct oz_hcd *ozhcd, int status)
{
	if (ozhcd) {
		struct oz_urb_link *urbl;
		while (!list_empty(&ozhcd->orphanage)) {
			urbl = list_first_entry(&ozhcd->orphanage,
				struct oz_urb_link, link);
			list_del(&urbl->link);
			oz_complete_urb(ozhcd->hcd, urbl->urb, status);
			oz_free_urb_link(urbl);
		}
	}
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static int oz_hcd_start(struct usb_hcd *hcd)
{
	hcd->power_budget = 200;
	hcd->state = HC_STATE_RUNNING;
	hcd->uses_new_polling = 1;
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static void oz_hcd_stop(struct usb_hcd *hcd)
{
	oz_trace("%s:\n", __func__);
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static void oz_hcd_shutdown(struct usb_hcd *hcd)
{
	oz_trace("%s:\n", __func__);
}
/*------------------------------------------------------------------------------
 * Called to queue an urb for the device.
 * This function should return a non-zero error code if it fails the urb but
 * should not call usb_hcd_giveback_urb().
 * Context: any
 */
static int oz_hcd_urb_enqueue(struct usb_hcd *hcd, struct urb *urb,
				gfp_t mem_flags)
{
	struct oz_hcd *ozhcd = oz_hcd_private(hcd);
	int rc = 0;
	int port_ix;
	struct oz_port *port;
	unsigned long irq_state;
	struct oz_urb_link *urbl;
	if (unlikely(ozhcd == NULL)) {
		oz_trace("Refused urb(%p) not ozhcd.\n", urb);
		return -ENODEV;
	}
	if (unlikely(hcd->state != HC_STATE_RUNNING)) {
		oz_trace("Refused urb(%p) not running.\n", urb);
		return -ENODEV;
	}
	port_ix = oz_get_port_from_addr(ozhcd, (usb_pipedevice(urb->pipe)));
	if (port_ix < 0)
		return -ENODEV;
	port =  &ozhcd->ports[port_ix];
	if (port == NULL)
		return -ENODEV;
	if ((port->flags & OZ_PORT_F_PRESENT) == 0) {
		oz_trace("Refusing URB port_ix = %d devnum = %d\n",
			port_ix, urb->dev->devnum);
		return -ENODEV;
	}
	urb->hcpriv = port;
	/* Put request in queue for processing by tasklet.
	 */
	urbl = oz_alloc_urb_link();
	if (unlikely(urbl == NULL))
		return -ENOMEM;
	urbl->urb = urb;
	spin_lock_irqsave(&g_tasklet_lock, irq_state);
	oz_trace_urb_in(urb);
	rc = usb_hcd_link_urb_to_ep(hcd, urb);
	if (unlikely(rc)) {
		spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
		oz_free_urb_link(urbl);
		return rc;
	}
	list_add_tail(&urbl->link, &ozhcd->urb_pending_list);
	spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
	tasklet_schedule(&g_urb_process_tasklet);
	atomic_inc(&g_pending_urbs);
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: tasklet
 */
static struct oz_urb_link *oz_remove_urb(struct oz_endpoint *ep,
				struct urb *urb)
{
	struct oz_urb_link *urbl = NULL;
	struct list_head *e;
	if (unlikely(ep == NULL))
		return 0;
	list_for_each(e, &ep->urb_list) {
		urbl = container_of(e, struct oz_urb_link, link);
		if (urbl->urb == urb) {
			list_del_init(e);
			if (usb_pipeisoc(urb->pipe)) {
				oz_trace_msg(M, "oz_remove_urb:%p\n", urb);
				ep->credit -= urb->number_of_packets;
				if (ep->credit < 0)
					ep->credit = 0;
			}
			return urbl;
		}
	}
	return 0;
}
/*------------------------------------------------------------------------------
 * Called to dequeue a previously submitted urb for the device.
 * Context: any
 */
static int oz_hcd_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	struct oz_hcd *ozhcd = oz_hcd_private(hcd);
	struct oz_urb_link *urbl = NULL;
	int rc;
	unsigned long irq_state;

	urbl = oz_alloc_urb_link();
	if (unlikely(urbl == NULL))
		return -ENOMEM;
	spin_lock_irqsave(&g_tasklet_lock, irq_state);
	/* The following function checks the urb is still in the queue
	 * maintained by the core and that the unlinked field is zero.
	 * If both are true the function sets the unlinked field and returns
	 * zero. Otherwise it returns an error.
	 */
	rc = usb_hcd_check_unlink_urb(hcd, urb, status);
	/* We have to check we haven't completed the urb or are about
	 * to complete it. When we do we set hcpriv to 0 so if this has
	 * already happened we don't put the urb in the cancel queue.
	 */
	if ((rc == 0) && urb->hcpriv) {
		urbl->urb = urb;
		urbl->port = (struct oz_port *)urb->hcpriv;
		urbl->ep_num = usb_pipeendpoint(urb->pipe);
		if (usb_pipein(urb->pipe))
			urbl->ep_num |= USB_DIR_IN;
		list_add_tail(&urbl->link, &ozhcd->urb_cancel_list);
		spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
		tasklet_schedule(&g_urb_cancel_tasklet);
	} else {
		spin_unlock_irqrestore(&g_tasklet_lock, irq_state);
		oz_free_urb_link(urbl);
	}
	return rc;
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static void oz_hcd_endpoint_disable(struct usb_hcd *hcd,
				struct usb_host_endpoint *ep)
{
	oz_trace("%s:\n", __func__);
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static void oz_hcd_endpoint_reset(struct usb_hcd *hcd,
				struct usb_host_endpoint *ep)
{
	oz_trace("%s:\n", __func__);
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static int oz_hcd_get_frame_number(struct usb_hcd *hcd)
{
	return oz_usb_get_frame_number();
}
/*------------------------------------------------------------------------------
 * Context: softirq
 * This is called as a consquence of us calling usb_hcd_poll_rh_status() and we
 * always do that in softirq context.
 */
static int oz_hcd_hub_status_data(struct usb_hcd *hcd, char *buf)
{
	struct oz_hcd *ozhcd = oz_hcd_private(hcd);
	int i;

	buf[0] = 0;
	buf[1] = 0;
	spin_lock_bh(&ozhcd->hcd_lock);
	for (i = 0; i < OZ_NB_PORTS; i++) {
		if (ozhcd->ports[i].flags & OZ_PORT_F_CHANGED) {
			if (i < 7)
				buf[0] |= 1<<(i+1);
			else
				buf[1] |= 1<<(i-7);
		}
	}
	oz_trace_msg(H, "HUBSTS : %02X%02X\n",
			(unsigned char)(buf[1]), (unsigned char)(buf[0]));
	spin_unlock_bh(&ozhcd->hcd_lock);
	if (buf[1] != 0 || buf[0] != 0)
		return 2;
	else
		return 0;
}
/*------------------------------------------------------------------------------
 * Context: process
 */
static void oz_get_hub_descriptor(struct usb_hcd *hcd,
				struct usb_hub_descriptor *desc)
{
	memset(desc, 0, sizeof(*desc));
	desc->bDescriptorType = 0x29;
	desc->bDescLength = 9;
	desc->wHubCharacteristics = (__force __u16)
			__constant_cpu_to_le16(0x0001);
	desc->bNbrPorts = OZ_NB_PORTS;
}
/*------------------------------------------------------------------------------
 * Context: process
 */
static int oz_set_port_feature(struct usb_hcd *hcd, u16 wvalue, u16 windex)
{
	struct oz_port *port;
	int err = 0;
	u8 port_id = (u8)windex;
	struct oz_hcd *ozhcd = oz_hcd_private(hcd);
	unsigned set_bits = 0;
	unsigned clear_bits = 0;
	void *hpd;

	if ((port_id < 1) || (port_id > OZ_NB_PORTS))
		return -ENODEV;
	port = &ozhcd->ports[port_id-1];
	switch (wvalue) {
	case USB_PORT_FEAT_CONNECTION:
		oz_trace("USB_PORT_FEAT_CONNECTION\n");
		break;
	case USB_PORT_FEAT_ENABLE:
		oz_trace("USB_PORT_FEAT_ENABLE\n");
		break;
	case USB_PORT_FEAT_SUSPEND:
		oz_trace("USB_PORT_FEAT_SUSPEND\n");
		break;
	case USB_PORT_FEAT_OVER_CURRENT:
		oz_trace("USB_PORT_FEAT_OVER_CURRENT\n");
		break;
	case USB_PORT_FEAT_RESET:
		oz_trace("USB_PORT_FEAT_RESET\n");
		set_bits = USB_PORT_STAT_ENABLE | (USB_PORT_STAT_C_RESET<<16);
		clear_bits = USB_PORT_STAT_RESET;
		ozhcd->ports[port_id-1].bus_addr = 0;
		hpd = oz_claim_hpd(&ozhcd->ports[port_id-1]);
		if (hpd != NULL) {
			oz_usb_reset_device(hpd);
			oz_usb_put(hpd);
		}
		break;
	case USB_PORT_FEAT_POWER:
		oz_trace("USB_PORT_FEAT_POWER\n");
		set_bits |= USB_PORT_STAT_POWER;
		break;
	case USB_PORT_FEAT_LOWSPEED:
		oz_trace("USB_PORT_FEAT_LOWSPEED\n");
		break;
	case USB_PORT_FEAT_C_CONNECTION:
		oz_trace("USB_PORT_FEAT_C_CONNECTION\n");
		break;
	case USB_PORT_FEAT_C_ENABLE:
		oz_trace("USB_PORT_FEAT_C_ENABLE\n");
		break;
	case USB_PORT_FEAT_C_SUSPEND:
		oz_trace("USB_PORT_FEAT_C_SUSPEND\n");
		break;
	case USB_PORT_FEAT_C_OVER_CURRENT:
		oz_trace("USB_PORT_FEAT_C_OVER_CURRENT\n");
		break;
	case USB_PORT_FEAT_C_RESET:
		oz_trace("USB_PORT_FEAT_C_RESET\n");
		break;
	case USB_PORT_FEAT_TEST:
		oz_trace("USB_PORT_FEAT_TEST\n");
		break;
	case USB_PORT_FEAT_INDICATOR:
		oz_trace("USB_PORT_FEAT_INDICATOR\n");
		break;
	default:
		oz_trace("Other %d\n", wvalue);
		break;
	}
	if (set_bits || clear_bits) {
		spin_lock_bh(&port->port_lock);
		port->status &= ~clear_bits;
		port->status |= set_bits;
		spin_unlock_bh(&port->port_lock);
	}

	oz_trace_msg(H, "Port[%d]: %08X\n", port_id,
		ozhcd->ports[port_id-1].status);
	return err;
}
/*------------------------------------------------------------------------------
 * Context: process
 */
static int oz_clear_port_feature(struct usb_hcd *hcd, u16 wvalue, u16 windex)
{
	struct oz_port *port;
	int err = 0;
	u8 port_id = (u8)windex;
	struct oz_hcd *ozhcd = oz_hcd_private(hcd);
	unsigned clear_bits = 0;

	if ((port_id < 1) || (port_id > OZ_NB_PORTS))
		return -ENODEV;
	port = &ozhcd->ports[port_id-1];
	switch (wvalue) {
	case USB_PORT_FEAT_CONNECTION:
		oz_trace("USB_PORT_FEAT_CONNECTION\n");
		break;
	case USB_PORT_FEAT_ENABLE:
		oz_trace("USB_PORT_FEAT_ENABLE\n");
		clear_bits = USB_PORT_STAT_ENABLE;
		break;
	case USB_PORT_FEAT_SUSPEND:
		oz_trace("USB_PORT_FEAT_SUSPEND\n");
		break;
	case USB_PORT_FEAT_OVER_CURRENT:
		oz_trace("USB_PORT_FEAT_OVER_CURRENT\n");
		break;
	case USB_PORT_FEAT_RESET:
		oz_trace("USB_PORT_FEAT_RESET\n");
		break;
	case USB_PORT_FEAT_POWER:
		oz_trace("USB_PORT_FEAT_POWER\n");
		clear_bits |= USB_PORT_STAT_POWER;
		break;
	case USB_PORT_FEAT_LOWSPEED:
		oz_trace("USB_PORT_FEAT_LOWSPEED\n");
		break;
	case USB_PORT_FEAT_C_CONNECTION:
		oz_trace("USB_PORT_FEAT_C_CONNECTION\n");
		clear_bits = (USB_PORT_STAT_C_CONNECTION << 16);
		spin_lock_bh(&port->port_lock);
		port->flags |= OZ_PORT_F_CHANGED;
		spin_unlock_bh(&port->port_lock);

		break;
	case USB_PORT_FEAT_C_ENABLE:
		oz_trace("USB_PORT_FEAT_C_ENABLE\n");
		clear_bits = (USB_PORT_STAT_C_ENABLE << 16);
		break;
	case USB_PORT_FEAT_C_SUSPEND:
		oz_trace("USB_PORT_FEAT_C_SUSPEND\n");
		break;
	case USB_PORT_FEAT_C_OVER_CURRENT:
		oz_trace("USB_PORT_FEAT_C_OVER_CURRENT\n");
		break;
	case USB_PORT_FEAT_C_RESET:
		oz_trace("USB_PORT_FEAT_C_RESET\n");
		clear_bits = (USB_PORT_FEAT_C_RESET << 16);
		break;
	case USB_PORT_FEAT_TEST:
		oz_trace("USB_PORT_FEAT_TEST\n");
		break;
	case USB_PORT_FEAT_INDICATOR:
		oz_trace("USB_PORT_FEAT_INDICATOR\n");
		break;
	default:
		oz_trace("Other %d\n", wvalue);
		break;
	}
	if (clear_bits) {
		spin_lock_bh(&port->port_lock);
		port->status &= ~clear_bits;
		spin_unlock_bh(&port->port_lock);
	}

	oz_trace_msg(H, "Port[%d]: %08X\n", port_id,
		ozhcd->ports[port_id-1].status);
	return err;
}
/*------------------------------------------------------------------------------
 * Context: process
 */
static int oz_get_port_status(struct usb_hcd *hcd, u16 windex, char *buf)
{
	struct oz_hcd *ozhcd;
	u32 status = 0;
	if ((windex < 1) || (windex > OZ_NB_PORTS))
		return -ENODEV;
	ozhcd = oz_hcd_private(hcd);
	ozhcd->ports[windex-1].flags &= ~OZ_PORT_F_CHANGED;
	status = ozhcd->ports[windex-1].status;
	put_unaligned(cpu_to_le32(status), (__le32 *)buf);
	oz_trace_msg(H, "Port[%d]: %08X\n", windex, status);
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: process
 */
static int oz_hcd_hub_control(struct usb_hcd *hcd, u16 req_type, u16 wvalue,
				u16 windex, char *buf, u16 wlength)
{
	int err = 0;

	oz_trace_msg(H, "HUBCTL: %04X %04X %04X %04X\n",
		req_type, wvalue, windex, wlength);

	switch (req_type) {
	case ClearHubFeature:
		oz_trace("ClearHubFeature: %d\n", req_type);
		break;
	case ClearPortFeature:
		err = oz_clear_port_feature(hcd, wvalue, windex);
		break;
	case GetHubDescriptor:
		oz_get_hub_descriptor(hcd, (struct usb_hub_descriptor *)buf);
		break;
	case GetHubStatus:
		oz_trace("GetHubStatus: req_type = 0x%x\n",
			req_type);
		put_unaligned(__constant_cpu_to_le32(0), (__le32 *)buf);
		break;
	case GetPortStatus:
		err = oz_get_port_status(hcd, windex, buf);
		break;
	case SetHubFeature:
		oz_trace("SetHubFeature: %d\n", req_type);
		break;
	case SetPortFeature:
		err = oz_set_port_feature(hcd, wvalue, windex);
		break;
	default:
		oz_trace("Other: %d\n", req_type);
		break;
	}
	return err;
}
/*------------------------------------------------------------------------------
 * Context: process
 */
static int oz_hcd_bus_suspend(struct usb_hcd *hcd)
{
	struct oz_hcd *ozhcd;

	ozhcd = oz_hcd_private(hcd);
	spin_lock_bh(&ozhcd->hcd_lock);
	hcd->state = HC_STATE_SUSPENDED;
	ozhcd->flags |= OZ_HDC_F_SUSPENDED;
	spin_unlock_bh(&ozhcd->hcd_lock);
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: process
 */
static int oz_hcd_bus_resume(struct usb_hcd *hcd)
{
	struct oz_hcd *ozhcd;

	ozhcd = oz_hcd_private(hcd);
	spin_lock_bh(&ozhcd->hcd_lock);
	ozhcd->flags &= ~OZ_HDC_F_SUSPENDED;
	hcd->state = HC_STATE_RUNNING;
	spin_unlock_bh(&ozhcd->hcd_lock);
	return 0;
}
/*------------------------------------------------------------------------------
 */
static void oz_plat_shutdown(struct platform_device *dev)
{
	oz_trace("%s:\n", __func__);
}
/*------------------------------------------------------------------------------
 * Context: process
 */
static int oz_plat_probe(struct platform_device *dev)
{
	int i;
	int err;
	struct usb_hcd *hcd;
	struct oz_hcd *ozhcd;

	hcd = usb_create_hcd(&g_oz_hc_drv, &dev->dev, dev_name(&dev->dev));
	if (hcd == NULL) {
		oz_trace("Failed to created hcd object\n");
		return -ENOMEM;
	}
	ozhcd = oz_hcd_private(hcd);
	memset(ozhcd, 0, sizeof(*ozhcd));
	INIT_LIST_HEAD(&ozhcd->urb_pending_list);
	INIT_LIST_HEAD(&ozhcd->urb_cancel_list);
	INIT_LIST_HEAD(&ozhcd->orphanage);
	ozhcd->hcd = hcd;
	ozhcd->conn_port = -1;
	spin_lock_init(&ozhcd->hcd_lock);
	for (i = 0; i < OZ_NB_PORTS; i++) {
		struct oz_port *port = &ozhcd->ports[i];
		port->ozhcd = ozhcd;
		port->flags = 0;
		port->status = 0;
		port->bus_addr = 0xff;
		spin_lock_init(&port->port_lock);
	}
	err = usb_add_hcd(hcd, -1, 0);
	if (err) {
		oz_trace("Failed to add hcd object\n");
		usb_put_hcd(hcd);
		return -1;
	}
	spin_lock_bh(&g_hcdlock);
	g_ozhcd = ozhcd;
	spin_unlock_bh(&g_hcdlock);
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static int oz_plat_remove(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);
	struct oz_hcd *ozhcd;

	if (hcd == NULL)
		return -1;
	ozhcd = oz_hcd_private(hcd);
	spin_lock_bh(&g_hcdlock);
	if (ozhcd == g_ozhcd)
		g_ozhcd = NULL;
	spin_unlock_bh(&g_hcdlock);
	oz_hcd_clear_orphanage(ozhcd, -ENODEV);
	usb_remove_hcd(hcd);
	usb_put_hcd(hcd);
	oz_empty_link_pool();
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static int oz_plat_suspend(struct platform_device *dev, pm_message_t msg)
{
	oz_trace("%s:\n", __func__);
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: unknown
 */
static int oz_plat_resume(struct platform_device *dev)
{
	oz_trace("%s:\n", __func__);
	return 0;
}
/*------------------------------------------------------------------------------
 * Context: process
 */
int oz_hcd_init(void)
{
	int err;
	if (usb_disabled())
		return -ENODEV;

	tasklet_init(&g_urb_process_tasklet, oz_urb_process_tasklet, 0);
	tasklet_init(&g_urb_cancel_tasklet, oz_urb_cancel_tasklet, 0);
	err = platform_driver_register(&g_oz_plat_drv);
	if (err)
		goto error;
	g_plat_dev = platform_device_alloc(OZ_PLAT_DEV_NAME, -1);
	if (g_plat_dev == NULL) {
		err = -ENOMEM;
		goto error1;
	}
	err = platform_device_add(g_plat_dev);
	if (err)
		goto error2;
	return 0;
error2:
	platform_device_put(g_plat_dev);
error1:
	platform_driver_unregister(&g_oz_plat_drv);
error:
	tasklet_disable(&g_urb_process_tasklet);
	tasklet_disable(&g_urb_cancel_tasklet);
	oz_trace("HCD Init failed: %d\n", err);
	return err;
}
/*------------------------------------------------------------------------------
 * Context: process
 */
void oz_hcd_term(void)
{
	msleep(OZ_HUB_DEBOUNCE_TIMEOUT);
	tasklet_kill(&g_urb_process_tasklet);
	tasklet_kill(&g_urb_cancel_tasklet);
	platform_device_unregister(g_plat_dev);
	platform_driver_unregister(&g_oz_plat_drv);
	oz_trace("Pending urbs:%d\n", atomic_read(&g_pending_urbs));
}

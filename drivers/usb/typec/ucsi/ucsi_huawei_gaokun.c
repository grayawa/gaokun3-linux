// SPDX-License-Identifier: GPL-2.0-only
/*
 * ucsi-huawei-gaokun - A UCSI driver for HUAWEI Matebook E Go
 *
 * Copyright (C) 2024-2025 Pengyu Luo <mitltlatltl@gmail.com>
 */

#include <drm/bridge/aux-bridge.h>
#include <linux/auxiliary_bus.h>
#include <linux/bitops.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_data/huawei-gaokun-ec.h>
#include <linux/string.h>
#include <linux/usb/pd_vdo.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/workqueue.h>

#include "ucsi.h"

#define EC_EVENT_UCSI	0x21
#define EC_EVENT_USB	0x22

#define GAOKUN_UCSI_REGISTER_DELAY	(3 * HZ)
#define GAOKUN_UCSI_RETRY_DELAY		(10 * HZ)
#define GAOKUN_UCSI_MAX_RETRIES		3
#define GAOKUN_UCSI_USB_EVENT_GRACE	(2 * HZ)

#define GAOKUN_CCX_MASK		GENMASK(1, 0)
#define GAOKUN_MUX_MASK		GENMASK(3, 2)

#define GAOKUN_DPAM_MASK	GENMASK(3, 0)
#define GAOKUN_HPD_STATE_MASK	BIT(4)
#define GAOKUN_HPD_IRQ_MASK	BIT(5)

#define GET_IDX(updt) (ffs(updt) - 1)

#define GAOKUN_UCSI_CMD_TRIES		5
#define GAOKUN_UCSI_CMD_RETRY_MS	200

#define CCX_TO_ORI(ccx) (++(ccx) % 3) /* convert ccx to enum typec_orientation */

#define CCX_TO_ORI(ccx) (++ccx % 3) /* convert ccx to enum typec_orientation */
/* Configuration Channel Extension */
enum gaokun_ucsi_ccx {
	USBC_CCX_NORMAL,
	USBC_CCX_REVERSE,
	USBC_CCX_NONE,
};

enum gaokun_ucsi_mux {
	USBC_MUX_NONE,
	USBC_MUX_USB_2L,
	USBC_MUX_DP_4L,
	USBC_MUX_USB_DP,
};

/* based on pmic_glink_altmode_pin_assignment */
enum gaokun_ucsi_dpam_pan {	/* DP Alt Mode Pin Assignments */
	USBC_DPAM_PAN_NONE,
	USBC_DPAM_PAN_A,	/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_B,	/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_C,	/* USBC_DPAM_PAN_C_REVERSE - 6 */
	USBC_DPAM_PAN_D,
	USBC_DPAM_PAN_E,
	USBC_DPAM_PAN_F,	/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_A_REVERSE,/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_B_REVERSE,/* Not supported after USB Type-C Standard v1.0b */
	USBC_DPAM_PAN_C_REVERSE,
	USBC_DPAM_PAN_D_REVERSE,
	USBC_DPAM_PAN_E_REVERSE,
	USBC_DPAM_PAN_F_REVERSE,/* Not supported after USB Type-C Standard v1.0b */
};

struct gaokun_ucsi_reg {
	u8 num_ports;
	u8 port_updt;
	u8 port_data[4];
	u8 checksum;
	u8 reserved;
} __packed;

#define GAOKUN_UCSI_BYTES_PER_PORT	2
#define GAOKUN_UCSI_MAX_PORTS \
	(sizeof_field(struct gaokun_ucsi_reg, port_data) / GAOKUN_UCSI_BYTES_PER_PORT)

struct gaokun_ucsi_port {
	spinlock_t lock; /* serializing port resource access */

	struct gaokun_ucsi *ucsi;
	struct auxiliary_device *bridge;
	struct typec_mux *typec_mux;
	struct typec_mux_state state;
	struct typec_altmode dp_alt;
	struct typec_displayport_data dp_data;

	int idx;
	enum gaokun_ucsi_ccx ccx;
	enum gaokun_ucsi_mux mux;
	u8 mode;
	u16 svid;
	u8 hpd_state;
	u8 hpd_irq;
	bool hpd_reported; /* last HPD state sent to the DP bridge */
};

struct gaokun_ucsi {
	struct gaokun_ec *ec;
	struct ucsi *ucsi;
	struct gaokun_ucsi_port *ports;
	struct device *dev;
	struct delayed_work work;
	struct delayed_work usb_work; /* stands in for a missing USB event */
	struct notifier_block nb;
	u16 version;
	u8 num_ports;
	u64 resend_cmd; /* GET_CONNECTOR_STATUS in flight, or 0 */
	bool registered;
	bool usb_events; /* the EC has sent a USB event */
	bool cmd_lost; /* the EC gave up resend_cmd */
};

/* -------------------------------------------------------------------------- */
/* For UCSI */

static int gaokun_ucsi_read_version(struct ucsi *ucsi, u16 *version)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(ucsi);

	*version = uec->version;

	return 0;
}

static int gaokun_ucsi_read_cci(struct ucsi *ucsi, u32 *cci)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(ucsi);
	u8 buf[GAOKUN_UCSI_READ_SIZE];
	int ret;

	ret = gaokun_ec_ucsi_read(uec->ec, buf);
	if (ret)
		return ret;

	memcpy(cci, buf, sizeof(*cci));

	return 0;
}

static int gaokun_ucsi_read_message_in(struct ucsi *ucsi,
				       void *val, size_t val_len)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(ucsi);
	u8 buf[GAOKUN_UCSI_READ_SIZE];
	int ret;

	ret = gaokun_ec_ucsi_read(uec->ec, buf);
	if (ret)
		return ret;

	memcpy(val, buf + GAOKUN_UCSI_CCI_SIZE,
	       min(val_len, GAOKUN_UCSI_MSGI_SIZE));

	return 0;
}

static int gaokun_ucsi_async_control(struct ucsi *ucsi, u64 command)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(ucsi);
	u8 buf[GAOKUN_UCSI_WRITE_SIZE] = {};

	memcpy(buf, &command, sizeof(command));

	return gaokun_ec_ucsi_write(uec->ec, buf);
}

/*
 * When the connector changes again while GET_CONNECTOR_STATUS for it is
 * pending, the EC sometimes gives the command up: it reports the change
 * instead of completing the command, or sets BUSY and keeps it set. It
 * does take the next command, so send this one again; it only reads state.
 * gaokun_ucsi_notify() ends the wait when it sees either.
 *
 * This relies on what the EC was seen to do. BUSY is a legal state, so a slow
 * command looks like a dropped one at first: the CCI is read again 200 ms
 * later, and a completion that arrived meanwhile is taken as the result. The
 * command is then sent again without a CANCEL first: a dropped command was
 * never seen to complete later, the resent one completed within 50 ms, and
 * how the EC handles CANCEL was not tried. Should a dropped command complete
 * after all, its COMMAND_COMPLETE could end the wait for the next command
 * early, and that command would read the wrong MESSAGE_IN.
 */
static int gaokun_ucsi_sync_control(struct ucsi *ucsi, u64 command, u32 *cci,
				    void *data, size_t size, void *msg_out,
				    size_t msg_out_size)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(ucsi);
	int tries = GAOKUN_UCSI_CMD_TRIES;
	u32 status;
	int ret;

	if (UCSI_COMMAND(command) != UCSI_GET_CONNECTOR_STATUS)
		return ucsi_sync_control_common(ucsi, command, cci, data, size,
						msg_out, msg_out_size);

	for (;;) {
		if (cci)
			*cci = 0;
		WRITE_ONCE(uec->cmd_lost, false);
		WRITE_ONCE(uec->resend_cmd, command);
		ret = ucsi_sync_control_common(ucsi, command, cci, data, size,
					       msg_out, msg_out_size);
		WRITE_ONCE(uec->resend_cmd, 0);
		if (!READ_ONCE(uec->cmd_lost))
			return ret;

		msleep(GAOKUN_UCSI_CMD_RETRY_MS);

		/* The EC may have completed the command after all */
		ret = gaokun_ucsi_read_cci(ucsi, &status);
		if (ret)
			return ret;
		if (status & UCSI_CCI_COMMAND_COMPLETE || !--tries)
			break;

		dev_dbg(uec->dev, "resending command %#llx\n", command);
	}

	/* Without COMMAND_COMPLETE, UCSI fails the command, or cancels it */
	if (cci)
		*cci = status;
	if (data && status & UCSI_CCI_COMMAND_COMPLETE)
		ret = gaokun_ucsi_read_message_in(ucsi, data, size);
	return ret;
}

static void gaokun_ucsi_update_connector(struct ucsi_connector *con)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(con->ucsi);

	if (con->num > uec->num_ports)
		return;

	con->typec_cap.orientation_aware = true;
}

static int gaokun_set_orientation(struct ucsi_connector *con,
				  struct gaokun_ucsi_port *port)
{
	enum typec_orientation orientation;
	enum gaokun_ucsi_ccx ccx;
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);
	ccx = port->ccx;
	spin_unlock_irqrestore(&port->lock, flags);

	orientation = CCX_TO_ORI(ccx);
	if (typec_get_orientation(con->port) != orientation)
		typec_set_orientation(con->port, orientation);
}

static void gaokun_ucsi_connector_status(struct ucsi_connector *con)
{
	struct gaokun_ucsi *uec = ucsi_get_drvdata(con->ucsi);
	int idx;

	idx = con->num - 1;
	if (con->num > uec->num_ports) {
		dev_warn(uec->dev, "set orientation out of range: con%d\n", idx);
		return;
	}

	if (gaokun_set_orientation(con, &uec->ports[idx]))
		dev_warn(uec->dev, "failed to set orientation for connector %d\n",
			 idx);
}

static const struct ucsi_operations gaokun_ucsi_ops = {
	.read_version = gaokun_ucsi_read_version,
	.read_cci = gaokun_ucsi_read_cci,
	.poll_cci = gaokun_ucsi_read_cci,
	.read_message_in = gaokun_ucsi_read_message_in,
	.sync_control = gaokun_ucsi_sync_control,
	.async_control = gaokun_ucsi_async_control,
	.update_connector = gaokun_ucsi_update_connector,
	.connector_status = gaokun_ucsi_connector_status,
};

/* -------------------------------------------------------------------------- */
/* For Altmode */

static void gaokun_ucsi_port_update(struct gaokun_ucsi_port *port,
				    const u8 *port_data)
{
	struct gaokun_ucsi *uec = port->ucsi;
	int offset = port->idx * GAOKUN_UCSI_BYTES_PER_PORT;
	unsigned long flags;
	u8 dcc, ddi;

	dcc = port_data[offset];
	ddi = port_data[offset + 1];

	spin_lock_irqsave(&port->lock, flags);

	port->ccx = FIELD_GET(GAOKUN_CCX_MASK, dcc);
	port->mux = FIELD_GET(GAOKUN_MUX_MASK, dcc);
	port->mode = FIELD_GET(GAOKUN_DPAM_MASK, ddi);
	port->hpd_state = FIELD_GET(GAOKUN_HPD_STATE_MASK, ddi);
	port->hpd_irq = FIELD_GET(GAOKUN_HPD_IRQ_MASK, ddi);

	switch (port->mode) {
	case USBC_DPAM_PAN_C:
	case USBC_DPAM_PAN_C_REVERSE:
		port->mode = DP_PIN_ASSIGN_C; /* correct it for usb later */
		break;
	case USBC_DPAM_PAN_D:
	case USBC_DPAM_PAN_D_REVERSE:
		port->mode = DP_PIN_ASSIGN_D;
		break;
	case USBC_DPAM_PAN_E:
	case USBC_DPAM_PAN_E_REVERSE:
		port->mode = DP_PIN_ASSIGN_E;
		break;
	case USBC_DPAM_PAN_NONE:
		port->mode = TYPEC_STATE_SAFE;
		break;
	default:
		dev_warn(uec->dev, "unknown mode %d\n", port->mode);
		break;
	}

	switch (port->mux) {
	case USBC_MUX_NONE:
		port->svid = 0;
		break;
	case USBC_MUX_USB_2L:
		port->svid = USB_SID_PD;
		port->mode = TYPEC_STATE_USB; /* same as PAN_C, correct it */
		break;
	case USBC_MUX_DP_4L:
	case USBC_MUX_USB_DP:
		port->svid = USB_SID_DISPLAYPORT;
		break;
	default:
		dev_warn(uec->dev, "unknown mux state %d\n", port->mux);
		break;
	}

	spin_unlock_irqrestore(&port->lock, flags);
}

static u8 gaokun_ucsi_valid_port_mask(const struct gaokun_ucsi *uec)
{
	if (!uec->num_ports)
		return 0;

	return GENMASK(uec->num_ports - 1, 0);
}

static int gaokun_ucsi_refresh(struct gaokun_ucsi *uec, u8 *port_mask)
{
	struct gaokun_ucsi_reg ureg;
	u8 valid_mask, updates;
	int ret, idx;

	ret = gaokun_ec_ucsi_get_reg(uec->ec, &ureg);
	if (ret)
		return ret;

	idx = GET_IDX(ureg.port_updt);

	if (idx < 0 || idx >= uec->num_ports)
		return GAOKUN_UCSI_NO_PORT_UPDATE;

	updates = ureg.port_updt & valid_mask;
	for (idx = 0; idx < uec->num_ports; idx++) {
		if (!(updates & BIT(idx)))
			continue;

		gaokun_ucsi_port_update(&uec->ports[idx], ureg.port_data);
	}

	*port_mask = updates;

	return 0;
}

static unsigned long gaokun_ucsi_typec_mux_mode(u8 mode)
{
	switch (mode) {
	case DP_PIN_ASSIGN_C:
		return TYPEC_DP_STATE_C;
	case DP_PIN_ASSIGN_D:
		return TYPEC_DP_STATE_D;
	case DP_PIN_ASSIGN_E:
		return TYPEC_DP_STATE_E;
	default:
		return TYPEC_STATE_SAFE;
	}
}

static void gaokun_ucsi_handle_altmode(struct gaokun_ucsi_port *port)
{
	struct gaokun_ucsi *uec = port->ucsi;
	struct ucsi_connector *con;
	int idx = port->idx, ret;

	/*
	 * For every typec port on this platform, the only mode-switch is
	 * controlled by its qmp combo phy which consumes svid and mode only.
	 */
	port->dp_alt.svid = port->svid;
	port->state.mode = port->mode;
	port->state.alt = &port->dp_alt;

	if (idx >= uec->num_ports) {
		dev_warn(uec->dev, "altmode port out of range: %d\n", idx);
		return;
	}

	/*
	 * Set the orientation from this event before the mode and HPD, so that
	 * the PHY lanes match the plug when DP starts link training. The
	 * .connector_status() callback sets it too, but only once UCSI's
	 * GET_CONNECTOR_STATUS completes, which can be later or time out. Not
	 * under the connector lock: in the EC notifier thread, UCSI may hold it
	 * while waiting for a command completion that only this thread can
	 * deliver. That leaves a small race with the callback: both set the
	 * orientation from port->ccx, so they only disagree if the callback
	 * read it just before this event changed it and writes after this, in
	 * which case the orientation stays stale until the next event on the
	 * port.
	 */
	if (idx < uec->ucsi->cap.num_connectors) {
		con = &uec->ucsi->connector[idx];
		if (con->port)
			gaokun_set_orientation(con, port);
	}

	ret = typec_mux_set(port->typec_mux, &port->state);
	if (ret)
		dev_err(uec->dev, "failed to set mux %d\n", ret);

	/*
	 * Report HPD while in DP alt mode, and once more when the port leaves
	 * it (unplug or a switch to USB), or the connector stays connected.
	 */
	if (port->bridge) {
		bool hpd = port->svid == USB_TYPEC_DP_SID && port->hpd_state;

		if (port->svid == USB_TYPEC_DP_SID || port->hpd_reported != hpd) {
			drm_aux_hpd_bridge_notify(&port->bridge->dev,
						  hpd ? connector_status_connected :
							connector_status_disconnected);
			port->hpd_reported = hpd;
		}
	}

	gaokun_ec_ucsi_pan_ack(uec->ec, port->idx);
}

/* Returns whether a port had an update */
static bool gaokun_ucsi_usb_notify_ind(struct gaokun_ucsi *uec)
{
	int idx;

	if (!uec->ucsi->connector) { /* slow to register */
		dev_err_ratelimited(uec->dev, "ucsi connector is not initialized yet\n");
		return false;
	}

	idx = gaokun_ucsi_refresh(uec);
	if (idx == GAOKUN_UCSI_NO_PORT_UPDATE) {
		gaokun_ec_ucsi_pan_ack(uec->ec, idx); /* ack directly if no update */
		return false;
	}

	gaokun_ucsi_handle_usb_mode(&uec->ports[idx]);
	return true;
}

/*
 * USB event is necessary for enabling altmode, the event should follow
 * UCSI event, if not after timeout(this notify may be disabled somehow),
 * then force to enable altmode. When inserting, 2 UCSI events (connector
 * change) are followed by USB events. Once we have received one USB event,
 * USB events are not blocked, and the fallback is no longer needed.
 */
static void gaokun_ucsi_usb_work(struct work_struct *work)
{
	struct gaokun_ucsi *uec = container_of(work, struct gaokun_ucsi,
					       usb_work.work);
	int i;

	dev_warn(uec->dev, "No USB EVENT, triggered by UCSI EVENT\n");

	/* One update per call; this stands in for several UCSI events */
	for (i = 0; i < uec->num_ports; i++)
		if (!gaokun_ucsi_usb_notify_ind(uec))
			break;
}

/* See gaokun_ucsi_sync_control() */
static void gaokun_ucsi_check_lost_cmd(struct gaokun_ucsi *uec, u32 cci)
{
	u64 cmd = READ_ONCE(uec->resend_cmd);

	if (!cmd || !test_bit(COMMAND_PENDING, &uec->ucsi->flags) ||
	    cci & UCSI_CCI_COMMAND_COMPLETE)
		return;

	if (cci & UCSI_CCI_BUSY ||
	    UCSI_CCI_CONNECTOR(cci) == UCSI_DEFAULT_GET_CONNECTOR_NUMBER(cmd)) {
		WRITE_ONCE(uec->cmd_lost, true);
		complete(&uec->ucsi->complete);
	}
}

static int gaokun_ucsi_notify(struct notifier_block *nb,
			      unsigned long action, void *data)
{
	u32 cci;
	struct gaokun_ucsi *uec = container_of(nb, struct gaokun_ucsi, nb);

	switch (action) {
	case EC_EVENT_USB:
		uec->usb_events = true;
		cancel_delayed_work_sync(&uec->usb_work);
		gaokun_ucsi_usb_notify_ind(uec);
		return NOTIFY_OK;

	case EC_EVENT_UCSI:
		ret = gaokun_ucsi_read_cci(uec->ucsi, &cci);
		if (ret)
			return NOTIFY_DONE;

		gaokun_ucsi_check_lost_cmd(uec, cci);
		ucsi_notify_common(uec->ucsi, cci);
		/*
		 * The USB event is delivered by the same EC IRQ thread that runs
		 * this notifier, so it cannot be waited for here. Freezable, so
		 * the fallback does not run during system suspend.
		 */
		if (UCSI_CCI_CONNECTOR(cci) && !uec->usb_events)
			queue_delayed_work(system_freezable_wq, &uec->usb_work,
					   2 * HZ);

		return NOTIFY_OK;

	default:
		return NOTIFY_DONE;
	}
}

static int gaokun_ucsi_ports_init(struct gaokun_ucsi *uec)
{
	struct gaokun_ucsi_port *ucsi_port;
	struct device *dev = uec->dev;
	struct fwnode_handle *fwnode;
	int i, ret, num_ports;
	u32 port;

	gaokun_ec_ucsi_get_reg(uec->ec, &ureg);
	num_ports = ureg.num_ports;
	if (num_ports > GAOKUN_UCSI_MAX_PORTS) {
		dev_warn(dev, "EC reports %d ports, the register holds data for %zu\n",
			 num_ports, GAOKUN_UCSI_MAX_PORTS);
		num_ports = GAOKUN_UCSI_MAX_PORTS;
	}
	uec->ports = devm_kcalloc(dev, num_ports, sizeof(*uec->ports),
				  GFP_KERNEL);
	if (!uec->ports)
		return -ENOMEM;
	uec->num_ports = num_ports;

	for (i = 0; i < num_ports; ++i) {
		ucsi_port = &uec->ports[i];
		ucsi_port->ccx = USBC_CCX_NONE;
		ucsi_port->idx = i;
		ucsi_port->ucsi = uec;
		spin_lock_init(&ucsi_port->lock);
	}

	device_for_each_child_node(dev, fwnode) {
		ret = fwnode_property_read_u32(fwnode, "reg", &port);
		if (ret < 0) {
			dev_err(dev, "missing reg property of %pOFn\n", fwnode);
			fwnode_handle_put(fwnode);
			return ret;
		}

		if (port >= num_ports) {
			dev_warn(dev, "invalid connector number %d, ignoring\n", port);
			continue;
		}

		ucsi_port = &uec->ports[port];
		ucsi_port->bridge = devm_drm_dp_hpd_bridge_alloc(dev, to_of_node(fwnode));
		if (IS_ERR(ucsi_port->bridge)) {
			fwnode_handle_put(fwnode);
			return PTR_ERR(ucsi_port->bridge);
		}

		ucsi_port->typec_mux = fwnode_typec_mux_get(fwnode);
		if (IS_ERR(ucsi_port->typec_mux)) {
			ret = PTR_ERR(ucsi_port->typec_mux);
			if (ret == -ENODEV) {
				ucsi_port->typec_mux = NULL;
			} else {
				fwnode_handle_put(fwnode);
				return ret;
			}
		}
	}
	for (i = 0; i < num_ports; i++) {
		if (!uec->ports[i].bridge)
			continue;

		ret = devm_drm_dp_hpd_bridge_add(dev, uec->ports[i].bridge);
		if (ret)
			return ret;
	}

	uec->ports_initialized = true;

	return 0;
}

static int gaokun_ucsi_ec_init(struct gaokun_ucsi *uec)
{
	struct gaokun_ucsi_reg ureg = {};
	int ret;

	ret = gaokun_ec_ucsi_get_reg(uec->ec, &ureg);
	if (ret)
		return ret;

	if (ureg.num_ports <= 0)
		return -ENODEV;

	if (ureg.num_ports > uec->num_ports)
		return -EINVAL;

	uec->num_ports = ureg.num_ports;

	return 0;
}

static void gaokun_ucsi_register_worker(struct work_struct *work)
{
	struct gaokun_ucsi *uec;
	struct ucsi *ucsi;
	int ret;

	uec = container_of(work, struct gaokun_ucsi, work.work);
	ucsi = uec->ucsi;

	ret = gaokun_ucsi_ports_init(uec);
	if (ret)
		goto retry;

	ret = gaokun_ucsi_ec_init(uec);
	if (ret)
		goto retry;

	ret = ucsi_register(ucsi);
	if (ret) {
		dev_err_probe(ucsi->dev, ret, "ucsi register failed\n");
		goto retry;
	}
	uec->ucsi_registered = true;

	ret = gaokun_ec_register_notify(uec->ec, &uec->nb);
	if (ret) {
		dev_err_probe(ucsi->dev, ret, "notifier register failed\n");
		ucsi_unregister(ucsi);
		uec->ucsi_registered = false;
		goto retry;
	}
	uec->notifier_registered = true;
	schedule_work(&uec->init_sync_work);

	return;

retry:
	if (++uec->register_retries > GAOKUN_UCSI_MAX_RETRIES) {
		dev_err(uec->dev, "giving up on UCSI registration after %u attempts\n",
			uec->register_retries);
		return;
	}

	dev_warn(uec->dev, "retrying UCSI registration in %u seconds (attempt %u/%u)\n",
		 GAOKUN_UCSI_RETRY_DELAY / HZ,
		 uec->register_retries, GAOKUN_UCSI_MAX_RETRIES);
	schedule_delayed_work(&uec->work, GAOKUN_UCSI_RETRY_DELAY);
}

static int gaokun_ucsi_probe(struct auxiliary_device *adev,
			     const struct auxiliary_device_id *id)
{
	struct gaokun_ec *ec = adev->dev.platform_data;
	struct device *dev = &adev->dev;
	struct gaokun_ucsi *uec;
	int ret;

	uec = devm_kzalloc(dev, sizeof(*uec), GFP_KERNEL);
	if (!uec)
		return -ENOMEM;

	uec->ec = ec;
	uec->dev = dev;
	uec->version = UCSI_VERSION_1_0;
	uec->last_connector_idx = GAOKUN_UCSI_NO_PORT_UPDATE;
	uec->nb.notifier_call = gaokun_ucsi_notify;

	INIT_DELAYED_WORK(&uec->work, gaokun_ucsi_register_worker);
	INIT_DELAYED_WORK(&uec->usb_work, gaokun_ucsi_usb_work);

	ret = gaokun_ucsi_ports_init(uec);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize UCSI ports\n");

	uec->ucsi = ucsi_create(dev, &gaokun_ucsi_ops);
	if (IS_ERR(uec->ucsi))
		return dev_err_probe(dev, PTR_ERR(uec->ucsi),
				     "failed to create UCSI instance\n");

	ucsi_set_drvdata(uec->ucsi, uec);
	auxiliary_set_drvdata(adev, uec);

	/*
	 * On MateBook E Go, early EC/UCSI traffic indirectly participates in
	 * the internal display bring-up chain. Defer registration until the
	 * platform settles to avoid booting with a dark panel.
	 */
	schedule_delayed_work(&uec->work, GAOKUN_UCSI_REGISTER_DELAY);

	return 0;
}

static void gaokun_ucsi_remove(struct auxiliary_device *adev)
{
	struct gaokun_ucsi *uec = auxiliary_get_drvdata(adev);
	int i;

	disable_delayed_work_sync(&uec->work);
	if (uec->registered)
		gaokun_ec_unregister_notify(uec->ec, &uec->nb);
	disable_delayed_work_sync(&uec->usb_work);
	if (uec->registered)
		ucsi_unregister(uec->ucsi);

	for (i = 0; i < uec->num_ports; ++i)
		typec_mux_put(uec->ports[i].typec_mux);

	ucsi_destroy(uec->ucsi);
}

static const struct auxiliary_device_id gaokun_ucsi_id_table[] = {
	{ .name = GAOKUN_MOD_NAME "." GAOKUN_DEV_UCSI, },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, gaokun_ucsi_id_table);

static struct auxiliary_driver gaokun_ucsi_driver = {
	.name = GAOKUN_DEV_UCSI,
	.id_table = gaokun_ucsi_id_table,
	.probe = gaokun_ucsi_probe,
	.remove = gaokun_ucsi_remove,
};

module_auxiliary_driver(gaokun_ucsi_driver);

MODULE_DESCRIPTION("HUAWEI Matebook E Go UCSI driver");
MODULE_LICENSE("GPL");

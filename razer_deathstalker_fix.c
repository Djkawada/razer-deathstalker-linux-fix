// SPDX-License-Identifier: GPL-2.0
/*
 * razer_deathstalker_fix.c - Fix 51.2s USB HID initialization hang, dead
 * NumLock LED, and hotplug timeout on Razer DeathStalker Essential 2014
 * (USB ID 1532:011f).
 *
 * Root cause:
 * The Razer DeathStalker Essential 2014 exposes 3 USB HID interfaces sharing
 * Control Endpoint 0 (EP0, bMaxPacketSize0 = 64):
 *   - Interface 0 (1.0): Boot Keyboard + LEDs (61-byte report descriptor)
 *   - Interface 1 (1.1): NKRO Keyboard + Consumer/System Control (179 bytes)
 *   - Interface 2 (1.2): Virtual Mouse + Vendor Feature Report (94 bytes)
 *
 * Three interacting firmware/kernel behaviors wedge the keyboard's EP0 state
 * machine on Linux:
 *   1. Sending HID_REQ_GET_REPORT on EP0 during usbhid_init_reports()
 *      destabilizes the MCU. Fixed by setting HID_QUIRK_NO_INIT_REPORTS |
 *      HID_QUIRK_NOGET before usbhid_start() runs.
 *   2. After reading a class-specific HID_DT_REPORT descriptor on EP0
 *      (bmRequestType = 0x81), the MCU leaves EP0 in a state where a subsequent
 *      SET_REPORT (LED update) or SET_IDLE wedges EP0 or drops LED output.
 *      Fixed by issuing a standard 18-byte GET_DESCRIPTOR(USB_DT_DEVICE)
 *      (bmRequestType = 0x80) on EP0 immediately before and after usbhid_parse().
 *   3. At the end of Interface 0's input registration, input_leds_connect()
 *      schedules hdev->led_work asynchronously on system_wq to send
 *      HID_REQ_SET_REPORT on EP0 at the exact same moment usb_set_configuration()
 *      begins probing Interface 1 (1.1) on EP0. Furthermore, Interface 1's
 *      179-byte report descriptor read occasionally drops one 64-byte packet
 *      (returning 115 bytes), and an immediate retry without resetting EP0
 *      hangs EP0 for 2 x 25.6s = 51.2s (-110 ETIMEDOUT).
 *      Fixed by flushing hdev->led_work before Interface 1 and Interface 2 are
 *      probed, pre-priming Interface 1's report descriptor, and resetting EP0
 *      with GET_DESCRIPTOR(USB_DT_DEVICE) between each step.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/device/bus.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define USB_VENDOR_ID_RAZER		0x1532
#define USB_DEVICE_ID_DEATHSTALKER_2014	0x011f

static int mode = 2;
module_param(mode, int, 0644);
MODULE_PARM_DESC(mode, "0 = enable main interface 1.0 + LEDs only, 2 = enable all interfaces (1.0, 1.1, 1.2) with full EP0 sync (default: 2)");

static struct hid_device *if0_hdev;
static struct hid_device *if1_hdev;
static struct usb_device *razer_udev;
static bool if1_ok;
static struct delayed_work led_resync_work;

static int reset_ep0_state(struct usb_device *udev, const char *stage)
{
	struct usb_device_descriptor *desc;
	int ret;

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
			      (USB_DT_DEVICE << 8), 0,
			      desc, sizeof(*desc), 200);
	if (ret == 0) {
		ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
				      USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
				      (USB_DT_DEVICE << 8), 0,
				      desc, sizeof(*desc), 200);
	}
	pr_debug("razer_deathstalker_fix: EP0 sync [%s]: ret=%d\n", stage, ret);
	kfree(desc);
	return ret;
}

static void razer_led_resync_fn(struct work_struct *work)
{
	struct hid_input *hidinput;

	if (!razer_udev || !if1_hdev)
		return;

	if (if1_hdev->led_work.func)
		flush_work(&if1_hdev->led_work);

	if (reset_ep0_state(razer_udev, "led_resync_before") > 0) {
		/* Re-trigger LED output event on the input device so hidinput_led_worker runs with current state */
		list_for_each_entry(hidinput, &if1_hdev->inputs, list) {
			if (hidinput->input && test_bit(LED_NUML, hidinput->input->led)) {
				input_event(hidinput->input, EV_LED, LED_NUML, 1);
				input_event(hidinput->input, EV_SYN, SYN_REPORT, 0);
			}
		}

		if (if1_hdev->led_work.func) {
			schedule_work(&if1_hdev->led_work);
			flush_work(&if1_hdev->led_work);
		}
		reset_ep0_state(razer_udev, "led_resync_after");
	}
}

static bool prime_if1_report_descriptor(struct usb_device *udev)
{
	u8 *buf;
	int ret;

	buf = kzalloc(0xb3, GFP_KERNEL);
	if (!buf)
		return false;

	ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      USB_REQ_GET_DESCRIPTOR,
			      USB_RECIP_INTERFACE | USB_TYPE_STANDARD | USB_DIR_IN,
			      (HID_DT_REPORT << 8), 1,
			      buf, 0xb3, 250);
	pr_debug("razer_deathstalker_fix: interface 1.1 GET_DESCRIPTOR(HID_DT_REPORT) ret=%d\n",
		 ret);
	kfree(buf);
	return (ret > 0);
}

static int razer_hid_bus_notify(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct device *dev = data;
	struct hid_device *hdev;
	struct usb_interface *intf;
	struct usb_device *udev;
	u8 ifnum = 0xff;

	if (action == BUS_NOTIFY_DEL_DEVICE && dev) {
		hdev = to_hid_device(dev);
		if (hdev == if0_hdev)
			if0_hdev = NULL;
		if (hdev == if1_hdev) {
			if1_hdev = NULL;
			razer_udev = NULL;
		}
		return NOTIFY_DONE;
	}

	if (action != BUS_NOTIFY_ADD_DEVICE || !dev)
		return NOTIFY_DONE;

	hdev = to_hid_device(dev);
	if (hdev->vendor == USB_VENDOR_ID_RAZER &&
	    hdev->product == USB_DEVICE_ID_DEATHSTALKER_2014) {
		hdev->quirks |= HID_QUIRK_NO_INIT_REPORTS | HID_QUIRK_NOGET;

		if (dev->parent) {
			intf = to_usb_interface(dev->parent);
			if (intf && intf->cur_altsetting)
				ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
			udev = interface_to_usbdev(intf);
			if (udev) {
				if (ifnum == 0) {
					if0_hdev = hdev;
				} else if (ifnum == 1) {
					if1_hdev = hdev;
					razer_udev = udev;
				}
				reset_ep0_state(udev, "after_usbhid_parse");
			}
		}

		/*
		 * Interface 0 (1.0, 61 bytes) and Interface 1 (1.1, 179 bytes) both
		 * declare LED output reports. Interface 1 is the NKRO gaming keyboard
		 * where actual user keystrokes originate in Linux.
		 *
		 * If both declare LEDs, Linux sends simultaneous SET_REPORT control
		 * transfers to wIndex=0 and wIndex=1 on EP0, wedging the Razer MCU.
		 * Furthermore, if Interface 1 has no LEDs, Wayland/Hyprland keystrokes
		 * never update Interface 0's LEDs because they are separate input devices.
		 *
		 * Mask Interface 0's LED output descriptor (offset 0x36: 0x91 0x02 -> 0x91 0x01)
		 * so ONLY Interface 1 handles LEDs. This eliminates the EP0 race condition
		 * during boot and ensures NumLock/CapsLock toggle correctly when keys are pressed.
		 */
		if (ifnum == 0 && hdev->dev_rdesc && hdev->dev_rsize == 61 &&
		    hdev->dev_rdesc[0x28] == 0x05 && hdev->dev_rdesc[0x29] == 0x08 &&
		    hdev->dev_rdesc[0x36] == 0x91 && hdev->dev_rdesc[0x37] == 0x02) {
			((u8 *)hdev->dev_rdesc)[0x37] = 0x01;
			dev_info(dev, "razer_deathstalker_fix: masked unused LED output on interface 1.0\n");
		}

		dev_info(dev, "razer_deathstalker_fix: applied NO_INIT_REPORTS | NOGET on if%u (quirks=0x%x)\n",
			 ifnum, hdev->quirks);
	}

	return NOTIFY_OK;
}

static struct notifier_block razer_hid_nb = {
	.notifier_call = razer_hid_bus_notify,
	.priority = INT_MAX,
};

static int razer_usb_bus_notify(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct device *dev = data;
	struct usb_interface *intf;
	struct usb_device *udev;
	u8 ifnum;
	int ep0_ret;

	if (action != BUS_NOTIFY_ADD_DEVICE)
		return NOTIFY_DONE;

	if (!dev || !dev->type || !dev->type->name)
		return NOTIFY_DONE;

	if (strcmp(dev->type->name, "usb_interface") != 0)
		return NOTIFY_DONE;

	intf = to_usb_interface(dev);
	if (!intf->cur_altsetting)
		return NOTIFY_DONE;

	udev = interface_to_usbdev(intf);
	if (!udev)
		return NOTIFY_DONE;

	if (le16_to_cpu(udev->descriptor.idVendor) != USB_VENDOR_ID_RAZER ||
	    le16_to_cpu(udev->descriptor.idProduct) != USB_DEVICE_ID_DEATHSTALKER_2014)
		return NOTIFY_DONE;

	ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	if (ifnum == 0) {
		if1_ok = false;
		reset_ep0_state(udev, "before_if0_usbhid_parse");
		dev_info(dev, "razer_deathstalker_fix: synchronized EP0 for primary keyboard interface 1.0\n");
		return NOTIFY_DONE;
	}

	if (ifnum == 1) {
		if (if0_hdev && if0_hdev->led_work.func)
			flush_work(&if0_hdev->led_work);
		ep0_ret = reset_ep0_state(udev, "after_if0_led_work");

		if (mode == 2 && ep0_ret > 0) {
			if1_ok = prime_if1_report_descriptor(udev);
			reset_ep0_state(udev, "after_if1_rdesc_prime");
		}
	}

	if (ifnum == 2 && if1_ok) {
		if (if1_hdev && if1_hdev->led_work.func)
			flush_work(&if1_hdev->led_work);
		reset_ep0_state(udev, "before_if2_probe");
		schedule_delayed_work(&led_resync_work, msecs_to_jiffies(250));
	}

	if ((ifnum == 1 || ifnum == 2) && if1_ok) {
		dev_info(dev, "razer_deathstalker_fix: synchronized EP0 for secondary interface 1.%u\n",
			 ifnum);
		return NOTIFY_OK;
	}

	if (ifnum == 1 || ifnum == 2) {
		dev_info(dev, "razer_deathstalker_fix: deauthorizing secondary interface 1.%u\n",
			 ifnum);
		intf->authorized = 0;
		schedule_delayed_work(&led_resync_work, msecs_to_jiffies(250));
	}

	return NOTIFY_OK;
}

static struct notifier_block razer_usb_nb = {
	.notifier_call = razer_usb_bus_notify,
	.priority = INT_MAX,
};

static const struct usb_device_id razer_dummy_ids[] = {
	{ }
};

static int razer_dummy_probe(struct usb_interface *intf,
			     const struct usb_device_id *id)
{
	return -ENODEV;
}

static void razer_dummy_disconnect(struct usb_interface *intf)
{
}

static struct usb_driver razer_dummy_driver = {
	.name = "razer_deathstalker_fix",
	.probe = razer_dummy_probe,
	.disconnect = razer_dummy_disconnect,
	.id_table = razer_dummy_ids,
};

static const struct bus_type *usb_bus_ptr;

static int razer_adopt_udev(struct usb_device *udev, void *data)
{
	struct usb_host_config *config;
	struct usb_interface *intf0, *intf1, *intf2;
	int ep0_ret;

	if (le16_to_cpu(udev->descriptor.idVendor) != USB_VENDOR_ID_RAZER ||
	    le16_to_cpu(udev->descriptor.idProduct) != USB_DEVICE_ID_DEATHSTALKER_2014)
		return 0;

	config = udev->actconfig;
	if (!config || config->desc.bNumInterfaces < 3)
		return 0;

	intf0 = config->interface[0];
	intf1 = config->interface[1];
	intf2 = config->interface[2];
	if (!intf0 || !intf1 || !intf2)
		return 0;

	if (intf0->dev.driver && intf1->dev.driver && intf2->dev.driver && if1_hdev)
		return 0;

	dev_info(&udev->dev,
		 "razer_deathstalker_fix: adopting existing device (1.0=%s, 1.1=%s, 1.2=%s)\n",
		 intf0->dev.driver ? "bound" : "unbound",
		 intf1->dev.driver ? "bound" : "unbound",
		 intf2->dev.driver ? "bound" : "unbound");

	reset_ep0_state(udev, "adopt_initial_check");

	/* Release any unpatched bindings first so unbind LED work finishes */
	if (intf2->dev.driver)
		device_release_driver(&intf2->dev);
	if (intf1->dev.driver)
		device_release_driver(&intf1->dev);
	if (intf0->dev.driver)
		device_release_driver(&intf0->dev);

	if1_ok = false;
	ep0_ret = reset_ep0_state(udev, "adopt_before_if0");
	if (ep0_ret < 0) {
		dev_warn(&udev->dev,
			 "razer_deathstalker_fix: EP0 unresponsive (%d), resetting USB device\n",
			 ep0_ret);
		intf0->authorized = 0;
		intf1->authorized = 0;
		intf2->authorized = 0;
		if (usb_lock_device_for_reset(udev, NULL) >= 0) {
			usb_reset_device(udev);
			usb_unlock_device(udev);
		}
		ep0_ret = reset_ep0_state(udev, "adopt_after_usb_reset");
	}

	intf0->authorized = 1;
	if (device_attach(&intf0->dev) < 0)
		dev_warn(&intf0->dev, "razer_deathstalker_fix: attach of 1.0 failed\n");

	if (if0_hdev && if0_hdev->led_work.func)
		flush_work(&if0_hdev->led_work);
	ep0_ret = reset_ep0_state(udev, "adopt_after_if0_led_work");

	if (mode == 2 && ep0_ret > 0) {
		if1_ok = prime_if1_report_descriptor(udev);
		reset_ep0_state(udev, "adopt_after_if1_rdesc_prime");
	}

	if (if1_ok) {
		intf1->authorized = 1;
		if (device_attach(&intf1->dev) < 0)
			dev_warn(&intf1->dev, "razer_deathstalker_fix: attach of 1.1 failed\n");

		if (if1_hdev && if1_hdev->led_work.func)
			flush_work(&if1_hdev->led_work);
		reset_ep0_state(udev, "adopt_before_if2_probe");
		intf2->authorized = 1;
		if (device_attach(&intf2->dev) < 0)
			dev_warn(&intf2->dev, "razer_deathstalker_fix: attach of 1.2 failed\n");
	} else {
		intf1->authorized = 0;
		intf2->authorized = 0;
	}

	schedule_delayed_work(&led_resync_work, msecs_to_jiffies(250));
	return 0;
}

static int __init razer_fix_init(void)
{
	int ret;

	INIT_DELAYED_WORK(&led_resync_work, razer_led_resync_fn);

	ret = usb_register(&razer_dummy_driver);
	if (ret)
		return ret;

	usb_bus_ptr = razer_dummy_driver.driver.bus;
	if (!usb_bus_ptr) {
		usb_deregister(&razer_dummy_driver);
		return -ENODEV;
	}

	ret = bus_register_notifier(&hid_bus_type, &razer_hid_nb);
	if (ret) {
		usb_deregister(&razer_dummy_driver);
		return ret;
	}

	ret = bus_register_notifier(usb_bus_ptr, &razer_usb_nb);
	if (ret) {
		bus_unregister_notifier(&hid_bus_type, &razer_hid_nb);
		usb_deregister(&razer_dummy_driver);
		return ret;
	}

	/* Clear any boot-time usbhid.quirks=0x1532:0x011f:0x00000004 (HID_QUIRK_IGNORE) */
	hid_quirks_exit(BUS_USB);

	pr_info("razer_deathstalker_fix: loaded (EP0 sync + mode=%d) for Razer DeathStalker Essential 2014 (1532:011f)\n",
		mode);

	/* Adopt any 1532:011f device already enumerated before module load */
	usb_for_each_dev(NULL, razer_adopt_udev);

	return 0;
}

static void __exit razer_fix_exit(void)
{
	cancel_delayed_work_sync(&led_resync_work);
	if (usb_bus_ptr)
		bus_unregister_notifier(usb_bus_ptr, &razer_usb_nb);
	bus_unregister_notifier(&hid_bus_type, &razer_hid_nb);
	usb_deregister(&razer_dummy_driver);
	pr_info("razer_deathstalker_fix: unloaded\n");
}

module_init(razer_fix_init);
module_exit(razer_fix_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Razer DeathStalker Linux Fix Contributors");
MODULE_DESCRIPTION("Fix 51.2s USB HID init timeout and dead NumLock LED on Razer DeathStalker Essential 2014 (1532:011f)");

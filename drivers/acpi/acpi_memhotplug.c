/*
 * Copyright (C) 2004 Intel Corporation <naveen.b.s@intel.com>
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * ACPI based HotPlug driver that supports Memory Hotplug
 * This driver fields notifications from firmware for memory add
 * and remove operations and alerts the VM of the affected memory
 * ranges.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/memory_hotplug.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <acpi/acpi_drivers.h>

#define ACPI_MEMORY_DEVICE_CLASS		"memory"
#define ACPI_MEMORY_DEVICE_HID			"PNP0C80"
#define ACPI_MEMORY_DEVICE_NAME			"Hotplug Mem Device"

#define _COMPONENT		ACPI_MEMORY_DEVICE_COMPONENT

#undef PREFIX
#define 	PREFIX		"ACPI:memory_hp:"

ACPI_MODULE_NAME("acpi_memhotplug");
MODULE_AUTHOR("Naveen B S <naveen.b.s@intel.com>");
MODULE_DESCRIPTION("Hotplug Mem Driver");
MODULE_LICENSE("GPL");

/* Memory Device States */
#define MEMORY_INVALID_STATE	0
#define MEMORY_POWER_ON_STATE	1
#define MEMORY_POWER_OFF_STATE	2

static int acpi_memory_device_add(struct acpi_device *device);
static int acpi_memory_device_remove(struct acpi_device *device, int type);

static const struct acpi_device_id memory_device_ids[] = {
	{ACPI_MEMORY_DEVICE_HID, 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, memory_device_ids);

static struct acpi_driver acpi_memory_device_driver = {
	.name = "acpi_memhotplug",
	.class = ACPI_MEMORY_DEVICE_CLASS,
	.ids = memory_device_ids,
	.ops = {
		.add = acpi_memory_device_add,
		.remove = acpi_memory_device_remove,
		},
};

struct acpi_memory_info {
	struct list_head list;
	u64 start_addr;		/* Memory Range start physical addr */
	u64 length;		/* Memory Range length */
	unsigned short caching;	/* memory cache attribute */
	unsigned short write_protect;	/* memory read/write attribute */
	unsigned int enabled:1;
	unsigned int failed:1;
};

struct acpi_memory_device {
	struct acpi_device * device;
	unsigned int state;	/* State of the memory device */
	struct list_head res_list;
};

static acpi_status
acpi_memory_get_resource(struct acpi_resource *resource, void *context)
{
	struct acpi_memory_device *mem_device = context;
	struct acpi_resource_address64 address64;
	struct acpi_memory_info *info, *new;
	acpi_status status;

	status = acpi_resource_to_address64(resource, &address64);
	if (ACPI_FAILURE(status) ||
	    (address64.resource_type != ACPI_MEMORY_RANGE))
		return AE_OK;

	list_for_each_entry(info, &mem_device->res_list, list) {
		/* Can we combine the resource range information? */
		if ((info->caching == address64.info.mem.caching) &&
		    (info->write_protect == address64.info.mem.write_protect) &&
		    (info->start_addr + info->length == address64.minimum)) {
			info->length += address64.address_length;
			return AE_OK;
		}
	}

	new = kzalloc(sizeof(struct acpi_memory_info), GFP_KERNEL);
	if (!new)
		return AE_ERROR;

	INIT_LIST_HEAD(&new->list);
	new->caching = address64.info.mem.caching;
	new->write_protect = address64.info.mem.write_protect;
	new->start_addr = address64.minimum;
	new->length = address64.address_length;
	list_add_tail(&new->list, &mem_device->res_list);

	return AE_OK;
}

static void
acpi_memory_free_device_resources(struct acpi_memory_device *mem_device)
{
	struct acpi_memory_info *info, *n;

	list_for_each_entry_safe(info, n, &mem_device->res_list, list)
		kfree(info);
	INIT_LIST_HEAD(&mem_device->res_list);
}

static int
acpi_memory_get_device_resources(struct acpi_memory_device *mem_device)
{
	acpi_status status;

	if (!list_empty(&mem_device->res_list))
		return 0;

	status = acpi_walk_resources(mem_device->device->handle, METHOD_NAME__CRS,
				     acpi_memory_get_resource, mem_device);
	if (ACPI_FAILURE(status)) {
		acpi_memory_free_device_resources(mem_device);
		return -EINVAL;
	}

	return 0;
}

static int
acpi_memory_get_device(acpi_handle handle,
		       struct acpi_memory_device **mem_device)
{
	acpi_status status;
	acpi_handle phandle;
	struct acpi_device *device = NULL;
	struct acpi_device *pdevice = NULL;
	int result;


	if (!acpi_bus_get_device(handle, &device) && device)
		goto end;

	status = acpi_get_parent(handle, &phandle);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "Cannot find acpi parent"));
		return -EINVAL;
	}

	/* Get the parent device */
	result = acpi_bus_get_device(phandle, &pdevice);
	if (result) {
		acpi_handle_warn(phandle, "Cannot get acpi bus device\n");
		return -EINVAL;
	}

	/*
	 * Now add the notified device.  This creates the acpi_device
	 * and invokes .add function
	 */
	result = acpi_bus_add(&device, pdevice, handle, ACPI_BUS_TYPE_DEVICE);
	if (result) {
		acpi_handle_warn(handle, "Cannot add acpi bus\n");
		return -EINVAL;
	}

      end:
	*mem_device = acpi_driver_data(device);
	if (!(*mem_device)) {
		dev_err(&device->dev, "driver data not found\n");
		return -ENODEV;
	}

	return 0;
}

static int acpi_memory_check_device(struct acpi_memory_device *mem_device)
{
	unsigned long long current_status;

	/* Get device present/absent information from the _STA */
	if (ACPI_FAILURE(acpi_evaluate_integer(mem_device->device->handle, "_STA",
					       NULL, &current_status)))
		return -ENODEV;
	/*
	 * Check for device status. Device should be
	 * present/enabled/functioning.
	 */
	if (!((current_status & ACPI_STA_DEVICE_PRESENT)
	      && (current_status & ACPI_STA_DEVICE_ENABLED)
	      && (current_status & ACPI_STA_DEVICE_FUNCTIONING)))
		return -ENODEV;

	return 0;
}

static int acpi_memory_enable_device(struct acpi_memory_device *mem_device)
{
	int result, num_enabled = 0;
	struct acpi_memory_info *info;
	int node;

	node = acpi_get_node(mem_device->device->handle);
	/*
	 * Tell the VM there is more memory here...
	 * Note: Assume that this function returns zero on success
	 * We don't have memory-hot-add rollback function,now.
	 * (i.e. memory-hot-remove function)
	 */
	list_for_each_entry(info, &mem_device->res_list, list) {
		if (info->enabled) { /* just sanity check...*/
			num_enabled++;
			continue;
		}
		/*
		 * If the memory block size is zero, please ignore it.
		 * Don't try to do the following memory hotplug flowchart.
		 */
		if (!info->length)
			continue;
		if (node < 0)
			node = memory_add_physaddr_to_nid(info->start_addr);

		result = add_memory(node, info->start_addr, info->length);

		/*
		 * If the memory block has been used by the kernel, add_memory()
		 * returns -EEXIST. If add_memory() returns the other error, it
		 * means that this memory block is not used by the kernel.
		 */
		if (result && result != -EEXIST) {
			info->failed = 1;
			continue;
		}

		if (!result)
			info->enabled = 1;
		/*
		 * Add num_enable even if add_memory() returns -EEXIST, so the
		 * device is bound to this driver.
		 */
		num_enabled++;
	}
	if (!num_enabled) {
		dev_err(&mem_device->device->dev, "add_memory failed\n");
		mem_device->state = MEMORY_INVALID_STATE;
		return -EINVAL;
	}
	/*
	 * Sometimes the memory device will contain several memory blocks.
	 * When one memory block is hot-added to the system memory, it will
	 * be regarded as a success.
	 * Otherwise if the last memory block can't be hot-added to the system
	 * memory, it will be failure and the memory device can't be bound with
	 * driver.
	 */
	return 0;
}

static int acpi_memory_remove_memory(struct acpi_memory_device *mem_device)
{
	int result = 0;
	struct acpi_memory_info *info, *n;

	list_for_each_entry_safe(info, n, &mem_device->res_list, list) {
		if (info->failed)
			/* The kernel does not use this memory block */
			continue;

		if (!info->enabled)
			/*
			 * The kernel uses this memory block, but it may be not
			 * managed by us.
			 */
			return -EBUSY;

		result = remove_memory(info->start_addr, info->length);
		if (result)
			return result;

		list_del(&info->list);
		kfree(info);
	}

	return result;
}

static void acpi_memory_device_notify(acpi_handle handle, u32 event, void *data)
{
	struct acpi_memory_device *mem_device;
	struct acpi_device *device;
	struct acpi_eject_event *ej_event = NULL;
	u32 ost_code = ACPI_OST_SC_NON_SPECIFIC_FAILURE; /* default */

	switch (event) {
	case ACPI_NOTIFY_BUS_CHECK:
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,
				  "\nReceived BUS CHECK notification for device\n"));
		/* Fall Through */
	case ACPI_NOTIFY_DEVICE_CHECK:
		if (event == ACPI_NOTIFY_DEVICE_CHECK)
			ACPI_DEBUG_PRINT((ACPI_DB_INFO,
					  "\nReceived DEVICE CHECK notification for device\n"));
		if (acpi_memory_get_device(handle, &mem_device)) {
			acpi_handle_err(handle, "Cannot find driver data\n");
			break;
		}

		ost_code = ACPI_OST_SC_SUCCESS;
		break;

	case ACPI_NOTIFY_EJECT_REQUEST:
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,
				  "\nReceived EJECT REQUEST notification for device\n"));

		if (acpi_bus_get_device(handle, &device)) {
			acpi_handle_err(handle, "Device doesn't exist\n");
			break;
		}
		mem_device = acpi_driver_data(device);
		if (!mem_device) {
			acpi_handle_err(handle, "Driver Data is NULL\n");
			break;
		}

		ej_event = kmalloc(sizeof(*ej_event), GFP_KERNEL);
		if (!ej_event) {
			pr_err(PREFIX "No memory, dropping EJECT\n");
			break;
		}

		ej_event->handle = handle;
		ej_event->event = ACPI_NOTIFY_EJECT_REQUEST;
		acpi_os_hotplug_execute(acpi_bus_hot_remove_device,
					(void *)ej_event);

		/* eject is performed asynchronously */
		return;
	default:
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,
				  "Unsupported event [0x%x]\n", event));

		/* non-hotplug event; possibly handled by other handler */
		return;
	}

	/* Inform firmware that the hotplug operation has completed */
	(void) acpi_evaluate_hotplug_ost(handle, event, ost_code, NULL);
	return;
}

static void acpi_memory_device_free(struct acpi_memory_device *mem_device)
{
	if (!mem_device)
		return;

	acpi_memory_free_device_resources(mem_device);
	kfree(mem_device);
}

static int acpi_memory_device_add(struct acpi_device *device)
{
	int result;
	struct acpi_memory_device *mem_device = NULL;


	if (!device)
		return -EINVAL;

	mem_device = kzalloc(sizeof(struct acpi_memory_device), GFP_KERNEL);
	if (!mem_device)
		return -ENOMEM;

	INIT_LIST_HEAD(&mem_device->res_list);
	mem_device->device = device;
	sprintf(acpi_device_name(device), "%s", ACPI_MEMORY_DEVICE_NAME);
	sprintf(acpi_device_class(device), "%s", ACPI_MEMORY_DEVICE_CLASS);
	device->driver_data = mem_device;

	/* Get the range from the _CRS */
	result = acpi_memory_get_device_resources(mem_device);
	if (result) {
		kfree(mem_device);
		return result;
	}

	/* Set the device state */
	mem_device->state = MEMORY_POWER_ON_STATE;

	pr_debug("%s\n", acpi_device_name(device));

	if (!acpi_memory_check_device(mem_device)) {
		/* call add_memory func */
		result = acpi_memory_enable_device(mem_device);
		if (result) {
			dev_err(&device->dev,
				"Error in acpi_memory_enable_device\n");
			acpi_memory_device_free(mem_device);
		}
	}
	return result;
}

static int acpi_memory_device_remove(struct acpi_device *device, int type)
{
	struct acpi_memory_device *mem_device = NULL;
	int result;

	if (!device || !acpi_driver_data(device))
		return -EINVAL;

	mem_device = acpi_driver_data(device);

	result = acpi_memory_remove_memory(mem_device);
	if (result)
		return result;

	acpi_memory_device_free(mem_device);

	return 0;
}

/*
 * Helper function to check for memory device
 */
static acpi_status is_memory_device(acpi_handle handle)
{
	char *hardware_id;
	acpi_status status;
	struct acpi_device_info *info;

	status = acpi_get_object_info(handle, &info);
	if (ACPI_FAILURE(status))
		return status;

	if (!(info->valid & ACPI_VALID_HID)) {
		kfree(info);
		return AE_ERROR;
	}

	hardware_id = info->hardware_id.string;
	if ((hardware_id == NULL) ||
	    (strcmp(hardware_id, ACPI_MEMORY_DEVICE_HID)))
		status = AE_ERROR;

	kfree(info);
	return status;
}

static acpi_status
acpi_memory_register_notify_handler(acpi_handle handle,
				    u32 level, void *ctxt, void **retv)
{
	acpi_status status;


	status = is_memory_device(handle);
	if (ACPI_FAILURE(status))
		return AE_OK;	/* continue */

	status = acpi_install_notify_handler(handle, ACPI_SYSTEM_NOTIFY,
					     acpi_memory_device_notify, NULL);
	/* continue */
	return AE_OK;
}

static acpi_status
acpi_memory_deregister_notify_handler(acpi_handle handle,
				      u32 level, void *ctxt, void **retv)
{
	acpi_status status;


	status = is_memory_device(handle);
	if (ACPI_FAILURE(status))
		return AE_OK;	/* continue */

	status = acpi_remove_notify_handler(handle,
					    ACPI_SYSTEM_NOTIFY,
					    acpi_memory_device_notify);

	return AE_OK;	/* continue */
}

static int __init acpi_memory_device_init(void)
{
	int result;
	acpi_status status;


	result = acpi_bus_register_driver(&acpi_memory_device_driver);

	if (result < 0)
		return -ENODEV;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
				     ACPI_UINT32_MAX,
				     acpi_memory_register_notify_handler, NULL,
				     NULL, NULL);

	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "walk_namespace failed"));
		acpi_bus_unregister_driver(&acpi_memory_device_driver);
		return -ENODEV;
	}

	return 0;
}

static void __exit acpi_memory_device_exit(void)
{
	acpi_status status;


	/*
	 * Adding this to un-install notification handlers for all the device
	 * handles.
	 */
	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
				     ACPI_UINT32_MAX,
				     acpi_memory_deregister_notify_handler, NULL,
				     NULL, NULL);

	if (ACPI_FAILURE(status))
		ACPI_EXCEPTION((AE_INFO, status, "walk_namespace failed"));

	acpi_bus_unregister_driver(&acpi_memory_device_driver);

	return;
}

module_init(acpi_memory_device_init);
module_exit(acpi_memory_device_exit);

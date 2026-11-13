// SPDX-License-Identifier: GPL-2.0-only
/* Parts are adapted from the Linux file named below, whose notice is:
 * Copyright (C) 2005 Mark M. Hoffman <mhoffman@lightlink.com>
 */
/* Linux's hardware monitoring class for drivers that register attribute
 * groups, following Linux cc9de5d4b5ab6459f5774946aa3195ad51236fa8
 * drivers/hwmon/hwmon.c. A hwmon device is registered under its parent with
 * its groups and driver data and named by an id; nothing reads sysfs, and no
 * device here has a label, an OF node or chip info. */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/string.h>

#define HWMON_ID_FORMAT "hwmon%d"

struct hwmon_device {
	const char *name;
	struct device dev;
	int id;
};

#define to_hwmon_device(d) container_of(d, struct hwmon_device, dev)

static DEFINE_IDA(hwmon_ida);

static void hwmon_dev_release(struct device *dev)
{
	kfree(to_hwmon_device(dev));
}

static const struct class hwmon_class = {
	.name = "hwmon",
	.dev_release = hwmon_dev_release,
};

struct device *hwmon_device_register_with_groups(struct device *dev, const char *name,
						 void *drvdata,
						 const struct attribute_group **groups)
{
	struct hwmon_device *hwdev;
	struct device *hdev;
	int err, id;

	if (!name)
		return ERR_PTR(-EINVAL);
	/* Complain about invalid characters in hwmon name attribute */
	if (!strlen(name) || strpbrk(name, "-* \t\n"))
		dev_warn(dev, "hwmon: '%s' is not a valid name attribute, please fix\n", name);
	id = ida_alloc(&hwmon_ida, GFP_KERNEL);
	if (id < 0)
		return ERR_PTR(id);
	hwdev = kzalloc_obj(*hwdev);
	if (!hwdev) {
		ida_free(&hwmon_ida, (unsigned int)id);
		return ERR_PTR(-ENOMEM);
	}
	hdev = &hwdev->dev;
	hdev->groups = groups;
	hwdev->name = name;
	hwdev->id = id;
	hdev->class = &hwmon_class;
	hdev->parent = dev;
	dev_set_drvdata(hdev, drvdata);
	err = dev_set_name(hdev, HWMON_ID_FORMAT, id);
	if (!err)
		err = device_register(hdev);
	if (err) {
		put_device(hdev);
		ida_free(&hwmon_ida, (unsigned int)id);
		return ERR_PTR(err);
	}
	return hdev;
}

void hwmon_device_unregister(struct device *dev)
{
	if (dev->class != &hwmon_class) {
		dev_dbg(dev->parent, "hwmon_device_unregister() failed: bad class ID!\n");
		return;
	}
	int id = to_hwmon_device(dev)->id;

	device_unregister(dev);
	ida_free(&hwmon_ida, (unsigned int)id);
}

#ifdef STELA_LINUX_HWMON_TEST
#include <stela/linux.h>
#include <stela/test.h>

static unsigned int warnings;

static void test_log(void *context, int level, const char *message)
{
	(void)context;
	if (level == LOGLEVEL_WARNING && strstr(message, "'a-b' is not a valid name attribute"))
		warnings++;
}

static void parent_release(struct device *dev)
{
	(void)dev;
}

int main(void)
{
	const struct stela_linux_services services = {.log = test_log};
	const struct stela_linux_owner owner = {.services = &services};
	stela_linux_enter(&owner);
	struct device parent = {.release = parent_release};
	device_initialize(&parent);
	assert(!dev_set_name(&parent, "gpu"));
	static const struct attribute_group *groups[] = {NULL};
	int data;

	/* Registered under its parent with its groups and data, named by id. */
	struct device *first = hwmon_device_register_with_groups(&parent, "amdgpu", &data, groups);
	assert(!IS_ERR(first) && !strcmp(dev_name(first), "hwmon0"));
	assert(first->parent == &parent && first->groups == groups);
	assert(dev_get_drvdata(first) == &data);
	/* A name with a dash warns, as in Linux, and still registers. */
	struct device *second = hwmon_device_register_with_groups(&parent, "a-b", NULL, NULL);
	assert(!IS_ERR(second) && !strcmp(dev_name(second), "hwmon1") && warnings == 1);
	assert(PTR_ERR(hwmon_device_register_with_groups(&parent, NULL, NULL, NULL)) == -EINVAL);
	/* Unregistering frees the device and its id for the next one. */
	hwmon_device_unregister(first);
	struct device *third = hwmon_device_register_with_groups(&parent, "amdgpu", NULL, NULL);
	assert(!IS_ERR(third) && !strcmp(dev_name(third), "hwmon0"));
	hwmon_device_unregister(third);
	hwmon_device_unregister(second);
	put_device(&parent);
	stela_linux_exit();
	printf("Linux hwmon devices: registration under the parent, ids and unregistration passed\n");
	return 0;
}
#endif

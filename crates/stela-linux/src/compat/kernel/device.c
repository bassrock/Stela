// SPDX-License-Identifier: GPL-2.0-only
/* Parts are adapted from the Linux files named below, whose notices are:
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2006 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2006 Novell, Inc.
 */
/* Linux's device model, following Linux
 * cc9de5d4b5ab6459f5774946aa3195ad51236fa8 drivers/base/core.c, class.c and
 * devres.c. Stela's owners create and attach devices, and nothing reads
 * sysfs: adding a device only keeps its parent alive, a class is its name and
 * its attribute files do not exist. */
#include <linux/component.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <stela/device.h>
#include <stela/catalogue.h>
#include <unsupported.h>

static void device_release(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	struct stela_linux_device_retirement retired;
	if (!stela_linux_device_release_begin(dev, &retired))
		return;

	if (dev->release)
		dev->release(dev);
	else if (dev->type && dev->type->release)
		dev->type->release(dev);
	else if (dev->class && dev->class->dev_release)
		dev->class->dev_release(dev);
	else
		WARN(1, "a device has no release function\n");
	stela_linux_device_release_end(&retired);
}

static const struct kobj_type device_ktype = {
	.release = device_release,
};

void device_initialize(struct device *dev)
{
	kobject_init(&dev->kobj, &device_ktype);
	INIT_LIST_HEAD(&dev->dma_pools);
	mutex_init(&dev->mutex);
	spin_lock_init(&dev->devres_lock);
	INIT_LIST_HEAD(&dev->devres_head);
}

int dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list arguments;

	va_start(arguments, fmt);
	int error = kobject_set_name_vargs(&dev->kobj, fmt, arguments);
	va_end(arguments);
	return error;
}

struct device *get_device(struct device *dev)
{
	return dev ? kobj_to_dev(kobject_get(&dev->kobj)) : NULL;
}

void put_device(struct device *dev)
{
	if (dev)
		kobject_put(&dev->kobj);
}

int device_add(struct device *dev)
{
	if (dev->init_name) {
		int error = dev_set_name(dev, "%s", dev->init_name);

		if (error)
			return error;
		dev->init_name = NULL;
	}
	if (!dev_name(dev))
		return -EINVAL;
	struct device *parent = dev->parent;
	get_device(dev);
	get_device(parent);
	int error = stela_linux_device_inherit(dev);
	if (error) {
		put_device(dev);
		put_device(parent);
		return error;
	}
	return 0;
}

void device_del(struct device *dev)
{
	struct device *parent = dev->parent;
	stela_linux_device_close(dev);

	stela_linux_devres_release(dev);
	put_device(dev);
	put_device(parent);
}

int device_register(struct device *dev)
{
	device_initialize(dev);
	return device_add(dev);
}

void device_unregister(struct device *dev)
{
	device_del(dev);
	put_device(dev);
}

/* A driver's managed actions, undone last first when it detaches. */
struct managed_action {
	struct list_head node;
	void (*action)(void *);
	void *data;
};

int __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name)
{
	(void)name;
	struct managed_action *managed = kmalloc_obj(*managed);

	if (!managed)
		return -ENOMEM;
	managed->action = action;
	managed->data = data;
	list_add(&managed->node, &dev->devres_head);
	return 0;
}

void devm_release_action(struct device *dev, void (*action)(void *), void *data)
{
	struct managed_action *managed;

	list_for_each_entry(managed, &dev->devres_head, node)
		if (managed->action == action && managed->data == data) {
			list_del(&managed->node);
			action(data);
			kfree(managed);
			return;
		}
	WARN_ON(1);
}

/* Attribute files, following drivers/base/core.c. Stela has no sysfs, so a
 * file or group is only checked and, when managed, its removal recorded for
 * the driver's detach. */
int device_create_file(struct device *dev, const struct device_attribute *attr)
{
	int error = 0;

	if (dev) {
		WARN(((attr->attr.mode & S_IWUGO) && !(attr->store || attr->store_const)),
		     "Attribute %s: write permission without 'store'\n", attr->attr.name);
		WARN(((attr->attr.mode & S_IRUGO) && !(attr->show || attr->show_const)),
		     "Attribute %s: read permission without 'show'\n", attr->attr.name);
		error = sysfs_create_file(&dev->kobj, &attr->attr);
	}
	return error;
}

void device_remove_file(struct device *dev, const struct device_attribute *attr)
{
	if (dev)
		sysfs_remove_file(&dev->kobj, &attr->attr);
}

struct attr_group_devres {
	struct device *dev;
	const struct attribute_group *group;
};

static void devm_attr_group_remove(void *res)
{
	struct attr_group_devres *devres = res;

	dev_dbg(devres->dev, "%s: removing group %p\n", __func__, devres->group);
	sysfs_remove_group(&devres->dev->kobj, devres->group);
	kfree(devres);
}

int devm_device_add_group(struct device *dev, const struct attribute_group *grp)
{
	struct attr_group_devres *devres = kmalloc_obj(*devres);
	int error;

	if (!devres)
		return -ENOMEM;
	error = sysfs_create_group(&dev->kobj, grp);
	if (error) {
		kfree(devres);
		return error;
	}
	devres->dev = dev;
	devres->group = grp;
	/* Linux allocates the record with its removal; here the managed action
	 * is the allocation that can fail, so a failure undoes the group. */
	error = __devm_add_action(dev, devm_attr_group_remove, devres, "devm_attr_group_remove");
	if (error) {
		sysfs_remove_group(&dev->kobj, grp);
		kfree(devres);
	}
	return error;
}

void stela_linux_devres_release(struct device *dev)
{
	struct managed_action *managed, *next;

	list_for_each_entry_safe(managed, next, &dev->devres_head, node) {
		list_del(&managed->node);
		managed->action(managed->data);
		kfree(managed);
	}
}

int dev_err_probe(const struct device *dev, int err, const char *fmt, ...)
{
	va_list arguments;

	va_start(arguments, fmt);
	struct va_format vaf = {.fmt = fmt, .va = &arguments};

	if (err == -EPROBE_DEFER)
		dev_dbg(dev, "error %pe: %pV", ERR_PTR(err), &vaf);
	else
		dev_err(dev, "error %pe: %pV", ERR_PTR(err), &vaf);
	va_end(arguments);
	return err;
}

struct class *class_create(const char *name)
{
	struct class *class = kzalloc_obj(*class);

	if (!class)
		return ERR_PTR(-ENOMEM);
	class->name = name;
	return class;
}

void class_destroy(const struct class *class)
{
	kfree(class);
}

int class_create_file_ns(const struct class *class, const struct class_attribute *attr,
			 const struct ns_common *ns)
{
	(void)class;
	(void)attr;
	(void)ns;
	return 0;
}

void class_remove_file_ns(const struct class *class, const struct class_attribute *attr,
			  const struct ns_common *ns)
{
	(void)class;
	(void)attr;
	(void)ns;
}

ssize_t show_class_attr_string(const struct class *class, const struct class_attribute *attr,
			       char *buf)
{
	(void)class;
	return sprintf(buf, "%s\n", container_of(attr, struct class_attribute_string, attr)->str);
}

/* Components join an aggregate device when its master registers. Stela's
 * audio does not bind to GPUs' HDMI codecs through one, so none ever does. */
int component_add(struct device *dev, const struct component_ops *ops)
{
	(void)dev;
	(void)ops;
	return 0;
}

void component_del(struct device *dev, const struct component_ops *ops)
{
	(void)dev;
	(void)ops;
}

/* Runtime power dependencies, which only a bound audio component makes. */
STELA_UNSUPPORTED(struct device_link *, device_link_add, struct device *, struct device *, u32)

#ifdef STELA_LINUX_DEVICE_TEST
#include <stela/linux.h>
#include <stela/test.h>

static unsigned int released;
static char undone[4];
static unsigned int undone_count;

static void release(struct device *dev)
{
	(void)dev;
	released++;
}

static void undo(void *data)
{
	undone[undone_count++] = *(const char *)data;
}

static ssize_t value_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)dev;
	(void)attr;
	return sysfs_emit(buf, "1\n");
}
static DEVICE_ATTR_RO(value);
static struct attribute *test_attrs[] = {&dev_attr_value.attr, NULL};
static const struct attribute_group test_group = {.attrs = test_attrs};

static bool test_property_bool(const struct fwnode_handle *node, const char *name)
{
	return !node->secondary && !strcmp(name, "test-enabled");
}

static int test_property_int(const struct fwnode_handle *node, const char *name,
			     unsigned int size, void *out, size_t count)
{
	if (node->secondary || strcmp(name, "test-values"))
		return -EINVAL;

	u32 *values = out;
	assert(values && size == sizeof(*values) && count == 2);
	values[0] = 7;
	values[1] = 13;
	return 0;
}

static void test_properties(void)
{
	static const struct fwnode_operations ops = {
		.property_read_bool = test_property_bool,
		.property_read_int_array = test_property_int,
	};
	struct fwnode_handle secondary = {.ops = &ops};
	struct fwnode_handle primary = {.ops = &ops, .secondary = &secondary};
	const struct device dev = {.fwnode = &primary};
	u32 values[2] = {};

	assert(dev_fwnode(&dev) == &primary);
	assert(device_property_read_bool(&dev, "test-enabled"));
	assert(!device_property_read_bool(&dev, "absent"));
	assert(!device_property_read_u32_array(&dev, "test-values", values,
					       ARRAY_SIZE(values)));
	assert(values[0] == 7 && values[1] == 13);
}

int main(void)
{
	stela_linux_enter(&stela_linux_bare);
	test_properties();
	struct device dev = {.release = release};
	device_initialize(&dev);
	assert(get_device(&dev) == &dev && !get_device(NULL));
	put_device(&dev);
	assert(!released);
	put_device(NULL);
	put_device(&dev);
	assert(released == 1);

	/* A child keeps its parent; managed actions undo last first. */
	released = 0;
	struct device parent = {.release = release};
	struct device child = {.parent = &parent, .release = release};
	device_initialize(&parent);
	assert(device_register(&child) == -EINVAL);
	assert(!dev_set_name(&child, "child") && !device_add(&child));
	put_device(&parent);
	assert(!released);

	static const char a = 'a', b = 'b', c = 'c';
	assert(!__devm_add_action(&child, undo, (void *)&a, "a"));
	assert(!__devm_add_action(&child, undo, (void *)&b, "b"));
	assert(!__devm_add_action(&child, undo, (void *)&c, "c"));
	devm_release_action(&child, undo, (void *)&b);
	assert(undone_count == 1 && undone[0] == 'b');
	/* Attribute files do not exist: creating one or a managed group checks
	 * it, and the group's removal goes with the other managed actions. */
	assert(!device_create_file(&child, &dev_attr_value));
	device_remove_file(&child, &dev_attr_value);
	assert(!device_create_file(NULL, &dev_attr_value));
	device_remove_file(NULL, &dev_attr_value);
	assert(!devm_device_add_group(&child, &test_group));
	device_unregister(&child);
	assert(!memcmp(undone, "bca", 3));
	put_device(&child);
	assert(released == 2);

	struct class *class = class_create("test");
	assert(!IS_ERR(class) && !strcmp(class->name, "test"));
	class_destroy(class);
	stela_linux_exit();
	printf("Linux device references, managed actions and attribute files passed\n");
	return 0;
}
#endif

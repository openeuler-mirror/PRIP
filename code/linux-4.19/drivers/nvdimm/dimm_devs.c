/*
 * Copyright(c) 2013-2015 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <linux/ndctl.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include "nd-core.h"
#include "label.h"
#include "pmem.h"
#include "nd.h"

static DEFINE_IDA(dimm_ida);

/*
 * Retrieve bus and dimm handle and return if this bus supports
 * get_config_data commands
 */
int nvdimm_check_config_data(struct device *dev)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);

	if (!nvdimm->cmd_mask ||
	    !test_bit(ND_CMD_GET_CONFIG_DATA, &nvdimm->cmd_mask)) {
		if (test_bit(NDD_ALIASING, &nvdimm->flags))
			return -ENXIO;
		else
			return -ENOTTY;
	}

	return 0;
}

static int validate_dimm(struct nvdimm_drvdata *ndd)
{
	int rc;

	if (!ndd)
		return -EINVAL;

	rc = nvdimm_check_config_data(ndd->dev);
	if (rc)
		dev_dbg(ndd->dev, "%pf: %s error: %d\n",
				__builtin_return_address(0), __func__, rc);
	return rc;
}

/**
 * nvdimm_init_nsarea - determine the geometry of a dimm's namespace area
 * @nvdimm: dimm to initialize
 */
int nvdimm_init_nsarea(struct nvdimm_drvdata *ndd)
{
	struct nd_cmd_get_config_size *cmd = &ndd->nsarea;
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(ndd->dev);
	struct nvdimm_bus_descriptor *nd_desc;
	int rc = validate_dimm(ndd);
	int cmd_rc = 0;

	if (rc)
		return rc;

	if (cmd->config_size)
		return 0; /* already valid */

	memset(cmd, 0, sizeof(*cmd));
	nd_desc = nvdimm_bus->nd_desc;
	rc = nd_desc->ndctl(nd_desc, to_nvdimm(ndd->dev),
			ND_CMD_GET_CONFIG_SIZE, cmd, sizeof(*cmd), &cmd_rc);
	if (rc < 0)
		return rc;
	return cmd_rc;
}

int nvdimm_init_config_data(struct nvdimm_drvdata *ndd)
{
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(ndd->dev);
	int rc = validate_dimm(ndd), cmd_rc = 0;
	struct nd_cmd_get_config_data_hdr *cmd;
	struct nvdimm_bus_descriptor *nd_desc;
	u32 max_cmd_size, config_size;
	size_t offset;

	if (rc)
		return rc;

	if (ndd->data)
		return 0;

	if (ndd->nsarea.status || ndd->nsarea.max_xfer == 0
			|| ndd->nsarea.config_size < ND_LABEL_MIN_SIZE) {
		dev_dbg(ndd->dev, "failed to init config data area: (%d:%d)\n",
				ndd->nsarea.max_xfer, ndd->nsarea.config_size);
		return -ENXIO;
	}

	ndd->data = kvmalloc(ndd->nsarea.config_size, GFP_KERNEL);
	if (!ndd->data)
		return -ENOMEM;

	max_cmd_size = min_t(u32, PAGE_SIZE, ndd->nsarea.max_xfer);
	cmd = kzalloc(max_cmd_size + sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	nd_desc = nvdimm_bus->nd_desc;
	for (config_size = ndd->nsarea.config_size, offset = 0;
			config_size; config_size -= cmd->in_length,
			offset += cmd->in_length) {
		cmd->in_length = min(config_size, max_cmd_size);
		cmd->in_offset = offset;
		rc = nd_desc->ndctl(nd_desc, to_nvdimm(ndd->dev),
				ND_CMD_GET_CONFIG_DATA, cmd,
				cmd->in_length + sizeof(*cmd), &cmd_rc);
		if (rc < 0)
			break;
		if (cmd_rc < 0) {
			rc = cmd_rc;
			break;
		}
		memcpy(ndd->data + offset, cmd->out_buf, cmd->in_length);
	}
	dev_dbg(ndd->dev, "len: %zu rc: %d\n", offset, rc);
	kfree(cmd);

	return rc;
}

int nvdimm_set_config_data(struct nvdimm_drvdata *ndd, size_t offset,
		void *buf, size_t len)
{
	size_t max_cmd_size, buf_offset;
	struct nd_cmd_set_config_hdr *cmd;
	int rc = validate_dimm(ndd), cmd_rc = 0;
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(ndd->dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;

	if (rc)
		return rc;

	if (!ndd->data)
		return -ENXIO;

	if (offset + len > ndd->nsarea.config_size)
		return -ENXIO;

	max_cmd_size = min_t(u32, PAGE_SIZE, len);
	max_cmd_size = min_t(u32, max_cmd_size, ndd->nsarea.max_xfer);
	cmd = kzalloc(max_cmd_size + sizeof(*cmd) + sizeof(u32), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	for (buf_offset = 0; len; len -= cmd->in_length,
			buf_offset += cmd->in_length) {
		size_t cmd_size;

		cmd->in_offset = offset + buf_offset;
		cmd->in_length = min(max_cmd_size, len);
		memcpy(cmd->in_buf, buf + buf_offset, cmd->in_length);

		/* status is output in the last 4-bytes of the command buffer */
		cmd_size = sizeof(*cmd) + cmd->in_length + sizeof(u32);

		rc = nd_desc->ndctl(nd_desc, to_nvdimm(ndd->dev),
				ND_CMD_SET_CONFIG_DATA, cmd, cmd_size, &cmd_rc);
		if (rc < 0)
			break;
		if (cmd_rc < 0) {
			rc = cmd_rc;
			break;
		}
	}
	kfree(cmd);

	return rc;
}

void nvdimm_set_aliasing(struct device *dev)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);

	set_bit(NDD_ALIASING, &nvdimm->flags);
}

void nvdimm_set_locked(struct device *dev)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);

	set_bit(NDD_LOCKED, &nvdimm->flags);
}

void nvdimm_clear_locked(struct device *dev)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);

	clear_bit(NDD_LOCKED, &nvdimm->flags);
}

static void nvdimm_release(struct device *dev)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);

	ida_simple_remove(&dimm_ida, nvdimm->id);
	kfree(nvdimm);
}

static struct device_type nvdimm_device_type = {
	.name = "nvdimm",
	.release = nvdimm_release,
};

bool is_nvdimm(struct device *dev)
{
	return dev->type == &nvdimm_device_type;
}

struct nvdimm *to_nvdimm(struct device *dev)
{
	struct nvdimm *nvdimm = container_of(dev, struct nvdimm, dev);

	WARN_ON(!is_nvdimm(dev));
	return nvdimm;
}
EXPORT_SYMBOL_GPL(to_nvdimm);

struct nvdimm *nd_blk_region_to_dimm(struct nd_blk_region *ndbr)
{
	struct nd_region *nd_region = &ndbr->nd_region;
	struct nd_mapping *nd_mapping = &nd_region->mapping[0];

	return nd_mapping->nvdimm;
}
EXPORT_SYMBOL_GPL(nd_blk_region_to_dimm);

unsigned long nd_blk_memremap_flags(struct nd_blk_region *ndbr)
{
	/* pmem mapping properties are private to libnvdimm */
	return ARCH_MEMREMAP_PMEM;
}
EXPORT_SYMBOL_GPL(nd_blk_memremap_flags);

struct nvdimm_drvdata *to_ndd(struct nd_mapping *nd_mapping)
{
	struct nvdimm *nvdimm = nd_mapping->nvdimm;

	WARN_ON_ONCE(!is_nvdimm_bus_locked(&nvdimm->dev));

	return dev_get_drvdata(&nvdimm->dev);
}
EXPORT_SYMBOL(to_ndd);

void nvdimm_drvdata_release(struct kref *kref)
{
	struct nvdimm_drvdata *ndd = container_of(kref, typeof(*ndd), kref);
	struct device *dev = ndd->dev;
	struct resource *res, *_r;

	dev_dbg(dev, "trace\n");
	nvdimm_bus_lock(dev);
	for_each_dpa_resource_safe(ndd, res, _r)
		nvdimm_free_dpa(ndd, res);
	nvdimm_bus_unlock(dev);

	kvfree(ndd->data);
	kfree(ndd);
	put_device(dev);
}

void get_ndd(struct nvdimm_drvdata *ndd)
{
	kref_get(&ndd->kref);
}

void put_ndd(struct nvdimm_drvdata *ndd)
{
	if (ndd)
		kref_put(&ndd->kref, nvdimm_drvdata_release);
}

const char *nvdimm_name(struct nvdimm *nvdimm)
{
	return dev_name(&nvdimm->dev);
}
EXPORT_SYMBOL_GPL(nvdimm_name);

struct kobject *nvdimm_kobj(struct nvdimm *nvdimm)
{
	return &nvdimm->dev.kobj;
}
EXPORT_SYMBOL_GPL(nvdimm_kobj);

unsigned long nvdimm_cmd_mask(struct nvdimm *nvdimm)
{
	return nvdimm->cmd_mask;
}
EXPORT_SYMBOL_GPL(nvdimm_cmd_mask);

void *nvdimm_provider_data(struct nvdimm *nvdimm)
{
	if (nvdimm)
		return nvdimm->provider_data;
	return NULL;
}
EXPORT_SYMBOL_GPL(nvdimm_provider_data);

static ssize_t commands_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);
	int cmd, len = 0;

	if (!nvdimm->cmd_mask)
		return sprintf(buf, "\n");

	for_each_set_bit(cmd, &nvdimm->cmd_mask, BITS_PER_LONG)
		len += sprintf(buf + len, "%s ", nvdimm_cmd_name(cmd));
	len += sprintf(buf + len, "\n");
	return len;
}
static DEVICE_ATTR_RO(commands);

static ssize_t flags_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);

	return sprintf(buf, "%s%s\n",
			test_bit(NDD_ALIASING, &nvdimm->flags) ? "alias " : "",
			test_bit(NDD_LOCKED, &nvdimm->flags) ? "lock " : "");
}
static DEVICE_ATTR_RO(flags);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);

	/*
	 * The state may be in the process of changing, userspace should
	 * quiesce probing if it wants a static answer
	 */
	nvdimm_bus_lock(dev);
	nvdimm_bus_unlock(dev);
	return sprintf(buf, "%s\n", atomic_read(&nvdimm->busy)
			? "active" : "idle");
}
static DEVICE_ATTR_RO(state);

static ssize_t available_slots_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvdimm_drvdata *ndd = dev_get_drvdata(dev);
	ssize_t rc;
	u32 nfree;

	if (!ndd)
		return -ENXIO;

	nvdimm_bus_lock(dev);
	nfree = nd_label_nfree(ndd);
	if (nfree - 1 > nfree) {
		dev_WARN_ONCE(dev, 1, "we ate our last label?\n");
		nfree = 0;
	} else
		nfree--;
	rc = sprintf(buf, "%d\n", nfree);
	nvdimm_bus_unlock(dev);
	return rc;
}
static DEVICE_ATTR_RO(available_slots);

__weak ssize_t security_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);

	switch (nvdimm->sec.state) {
	case NVDIMM_SECURITY_DISABLED:
		return sprintf(buf, "disabled\n");
	case NVDIMM_SECURITY_UNLOCKED:
		return sprintf(buf, "unlocked\n");
	case NVDIMM_SECURITY_LOCKED:
		return sprintf(buf, "locked\n");
	case NVDIMM_SECURITY_FROZEN:
		return sprintf(buf, "frozen\n");
	case NVDIMM_SECURITY_OVERWRITE:
		return sprintf(buf, "overwrite\n");
	default:
		return -ENOTTY;
	}

	return -ENOTTY;
}

#define OPS							\
	C( OP_FREEZE,		"freeze",		1),	\
	C( OP_DISABLE,		"disable",		2),	\
	C( OP_UPDATE,		"update",		3),	\
	C( OP_ERASE,		"erase",		2),	\
	C( OP_OVERWRITE,	"overwrite",		2),	\
	C( OP_MASTER_UPDATE,	"master_update",	3),	\
	C( OP_MASTER_ERASE,	"master_erase",		2)
#undef C
#define C(a, b, c) a
enum nvdimmsec_op_ids { OPS };
#undef C
#define C(a, b, c) { b, c }
static struct {
	const char *name;
	int args;
} ops[] = { OPS };
#undef C

#define SEC_CMD_SIZE 32
#define KEY_ID_SIZE 10

static ssize_t __security_store(struct device *dev, const char *buf, size_t len)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);
	ssize_t rc;
	char cmd[SEC_CMD_SIZE+1], keystr[KEY_ID_SIZE+1],
		nkeystr[KEY_ID_SIZE+1];
	unsigned int key, newkey;
	int i;

	if (atomic_read(&nvdimm->busy))
		return -EBUSY;

	rc = sscanf(buf, "%"__stringify(SEC_CMD_SIZE)"s"
			" %"__stringify(KEY_ID_SIZE)"s"
			" %"__stringify(KEY_ID_SIZE)"s",
			cmd, keystr, nkeystr);
	if (rc < 1)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(ops); i++)
		if (sysfs_streq(cmd, ops[i].name))
			break;
	if (i >= ARRAY_SIZE(ops))
		return -EINVAL;
	if (ops[i].args > 1)
		rc = kstrtouint(keystr, 0, &key);
	if (rc >= 0 && ops[i].args > 2)
		rc = kstrtouint(nkeystr, 0, &newkey);
	if (rc < 0)
		return rc;

	if (i == OP_FREEZE) {
		dev_dbg(dev, "freeze\n");
		rc = nvdimm_security_freeze(nvdimm);
	} else if (i == OP_DISABLE) {
		dev_dbg(dev, "disable %u\n", key);
		rc = nvdimm_security_disable(nvdimm, key);
	} else if (i == OP_UPDATE) {
		dev_dbg(dev, "update %u %u\n", key, newkey);
		rc = nvdimm_security_update(nvdimm, key, newkey, NVDIMM_USER);
	} else if (i == OP_ERASE) {
		dev_dbg(dev, "erase %u\n", key);
		rc = nvdimm_security_erase(nvdimm, key, NVDIMM_USER);
	} else if (i == OP_OVERWRITE) {
		dev_dbg(dev, "overwrite %u\n", key);
		rc = nvdimm_security_overwrite(nvdimm, key);
	} else if (i == OP_MASTER_UPDATE) {
		dev_dbg(dev, "master_update %u %u\n", key, newkey);
		rc = nvdimm_security_update(nvdimm, key, newkey,
				NVDIMM_MASTER);
	} else if (i == OP_MASTER_ERASE) {
		dev_dbg(dev, "master_erase %u\n", key);
		rc = nvdimm_security_erase(nvdimm, key,
				NVDIMM_MASTER);
	} else
		return -EINVAL;

	if (rc == 0)
		rc = len;
	return rc;
}

static ssize_t security_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)

{
	ssize_t rc;

	/*
	 * Require all userspace triggered security management to be
	 * done while probing is idle and the DIMM is not in active use
	 * in any region.
	 */
	device_lock(dev);
	nvdimm_bus_lock(dev);
	wait_nvdimm_bus_probe_idle(dev);
	rc = __security_store(dev, buf, len);
	nvdimm_bus_unlock(dev);
	device_unlock(dev);

	return rc;
}
static DEVICE_ATTR_RW(security);

static struct attribute *nvdimm_attributes[] = {
	&dev_attr_state.attr,
	&dev_attr_flags.attr,
	&dev_attr_commands.attr,
	&dev_attr_available_slots.attr,
	&dev_attr_security.attr,
	NULL,
};

static umode_t nvdimm_visible(struct kobject *kobj, struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, typeof(*dev), kobj);
	struct nvdimm *nvdimm = to_nvdimm(dev);

	if (a != &dev_attr_security.attr)
		return a->mode;
	if (nvdimm->sec.state < 0)
		return 0;
	/* Are there any state mutation ops? */
	if (nvdimm->sec.ops->freeze || nvdimm->sec.ops->disable
			|| nvdimm->sec.ops->change_key
			|| nvdimm->sec.ops->erase
			|| nvdimm->sec.ops->overwrite)
		return a->mode;
	return 0444;
}

struct attribute_group nvdimm_attribute_group = {
	.attrs = nvdimm_attributes,
	.is_visible = nvdimm_visible,
};
EXPORT_SYMBOL_GPL(nvdimm_attribute_group);

struct nvdimm *__nvdimm_create(struct nvdimm_bus *nvdimm_bus,
		void *provider_data, const struct attribute_group **groups,
		unsigned long flags, unsigned long cmd_mask, int num_flush,
		struct resource *flush_wpq, const char *dimm_id,
		const struct nvdimm_security_ops *sec_ops)
{
	struct nvdimm *nvdimm = kzalloc(sizeof(*nvdimm), GFP_KERNEL);
	struct device *dev;

	if (!nvdimm)
		return NULL;

	nvdimm->id = ida_simple_get(&dimm_ida, 0, 0, GFP_KERNEL);
	if (nvdimm->id < 0) {
		kfree(nvdimm);
		return NULL;
	}

	nvdimm->dimm_id = dimm_id;
	nvdimm->provider_data = provider_data;
	nvdimm->flags = flags;
	nvdimm->cmd_mask = cmd_mask;
	nvdimm->num_flush = num_flush;
	nvdimm->flush_wpq = flush_wpq;
	atomic_set(&nvdimm->busy, 0);
	dev = &nvdimm->dev;
	dev_set_name(dev, "nmem%d", nvdimm->id);
	dev->parent = &nvdimm_bus->dev;
	dev->type = &nvdimm_device_type;
	dev->devt = MKDEV(nvdimm_major, nvdimm->id);
	dev->groups = groups;
	nvdimm->sec.ops = sec_ops;
	nvdimm->sec.overwrite_tmo = 0;
	INIT_DELAYED_WORK(&nvdimm->dwork, nvdimm_security_overwrite_query);
	/*
	 * Security state must be initialized before device_add() for
	 * attribute visibility.
	 */
	/* get security state and extended (master) state */
	nvdimm->sec.state = nvdimm_security_state(nvdimm, NVDIMM_USER);
	nvdimm->sec.ext_state = nvdimm_security_state(nvdimm, NVDIMM_MASTER);
	nd_device_register(dev);

	return nvdimm;
}
EXPORT_SYMBOL_GPL(__nvdimm_create);

int nvdimm_security_setup_events(struct nvdimm *nvdimm)
{
	nvdimm->sec.overwrite_state = sysfs_get_dirent(nvdimm->dev.kobj.sd,
			"security");
	if (!nvdimm->sec.overwrite_state)
		return -ENODEV;
	return 0;
}
EXPORT_SYMBOL_GPL(nvdimm_security_setup_events);

int nvdimm_in_overwrite(struct nvdimm *nvdimm)
{
	return test_bit(NDD_SECURITY_OVERWRITE, &nvdimm->flags);
}
EXPORT_SYMBOL_GPL(nvdimm_in_overwrite);

int nvdimm_security_freeze(struct nvdimm *nvdimm)
{
	int rc;

	WARN_ON_ONCE(!is_nvdimm_bus_locked(&nvdimm->dev));

	if (!nvdimm->sec.ops || !nvdimm->sec.ops->freeze)
		return -EOPNOTSUPP;

	if (nvdimm->sec.state < 0)
		return -EIO;

	if (test_bit(NDD_SECURITY_OVERWRITE, &nvdimm->flags)) {
		dev_warn(&nvdimm->dev, "Overwrite operation in progress.\n");
		return -EBUSY;
	}

	rc = nvdimm->sec.ops->freeze(nvdimm);
	nvdimm->sec.state = nvdimm_security_state(nvdimm, NVDIMM_USER);

	return rc;
}

int alias_dpa_busy(struct device *dev, void *data)
{
	resource_size_t map_end, blk_start, new;
	struct blk_alloc_info *info = data;
	struct nd_mapping *nd_mapping;
	struct nd_region *nd_region;
	struct nvdimm_drvdata *ndd;
	struct resource *res;
	int i;

	if (!is_memory(dev))
		return 0;

	nd_region = to_nd_region(dev);
	for (i = 0; i < nd_region->ndr_mappings; i++) {
		nd_mapping  = &nd_region->mapping[i];
		if (nd_mapping->nvdimm == info->nd_mapping->nvdimm)
			break;
	}

	if (i >= nd_region->ndr_mappings)
		return 0;

	ndd = to_ndd(nd_mapping);
	map_end = nd_mapping->start + nd_mapping->size - 1;
	blk_start = nd_mapping->start;

	/*
	 * In the allocation case ->res is set to free space that we are
	 * looking to validate against PMEM aliasing collision rules
	 * (i.e. BLK is allocated after all aliased PMEM).
	 */
	if (info->res) {
		if (info->res->start >= nd_mapping->start
				&& info->res->start < map_end)
			/* pass */;
		else
			return 0;
	}

 retry:
	/*
	 * Find the free dpa from the end of the last pmem allocation to
	 * the end of the interleave-set mapping.
	 */
	for_each_dpa_resource(ndd, res) {
		if (strncmp(res->name, "pmem", 4) != 0)
			continue;
		if ((res->start >= blk_start && res->start < map_end)
				|| (res->end >= blk_start
					&& res->end <= map_end)) {
			new = max(blk_start, min(map_end + 1, res->end + 1));
			if (new != blk_start) {
				blk_start = new;
				goto retry;
			}
		}
	}

	/* update the free space range with the probed blk_start */
	if (info->res && blk_start > info->res->start) {
		info->res->start = max(info->res->start, blk_start);
		if (info->res->start > info->res->end)
			info->res->end = info->res->start - 1;
		return 1;
	}

	info->available -= blk_start - nd_mapping->start;

	return 0;
}

/**
 * nd_blk_available_dpa - account the unused dpa of BLK region
 * @nd_mapping: container of dpa-resource-root + labels
 *
 * Unlike PMEM, BLK namespaces can occupy discontiguous DPA ranges, but
 * we arrange for them to never start at an lower dpa than the last
 * PMEM allocation in an aliased region.
 */
resource_size_t nd_blk_available_dpa(struct nd_region *nd_region)
{
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(&nd_region->dev);
	struct nd_mapping *nd_mapping = &nd_region->mapping[0];
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	struct blk_alloc_info info = {
		.nd_mapping = nd_mapping,
		.available = nd_mapping->size,
		.res = NULL,
	};
	struct resource *res;

	if (!ndd)
		return 0;

	device_for_each_child(&nvdimm_bus->dev, &info, alias_dpa_busy);

	/* now account for busy blk allocations in unaliased dpa */
	for_each_dpa_resource(ndd, res) {
		if (strncmp(res->name, "blk", 3) != 0)
			continue;
		info.available -= resource_size(res);
	}

	return info.available;
}

/**
 * nd_pmem_max_contiguous_dpa - For the given dimm+region, return the max
 *			   contiguous unallocated dpa range.
 * @nd_region: constrain available space check to this reference region
 * @nd_mapping: container of dpa-resource-root + labels
 */
resource_size_t nd_pmem_max_contiguous_dpa(struct nd_region *nd_region,
					   struct nd_mapping *nd_mapping)
{
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	struct nvdimm_bus *nvdimm_bus;
	resource_size_t max = 0;
	struct resource *res;

	/* if a dimm is disabled the available capacity is zero */
	if (!ndd)
		return 0;

	nvdimm_bus = walk_to_nvdimm_bus(ndd->dev);
	if (__reserve_free_pmem(&nd_region->dev, nd_mapping->nvdimm))
		return 0;
	for_each_dpa_resource(ndd, res) {
		if (strcmp(res->name, "pmem-reserve") != 0)
			continue;
		if (resource_size(res) > max)
			max = resource_size(res);
	}
	release_free_pmem(nvdimm_bus, nd_mapping);
	return max;
}

/**
 * nd_pmem_available_dpa - for the given dimm+region account unallocated dpa
 * @nd_mapping: container of dpa-resource-root + labels
 * @nd_region: constrain available space check to this reference region
 * @overlap: calculate available space assuming this level of overlap
 *
 * Validate that a PMEM label, if present, aligns with the start of an
 * interleave set and truncate the available size at the lowest BLK
 * overlap point.
 *
 * The expectation is that this routine is called multiple times as it
 * probes for the largest BLK encroachment for any single member DIMM of
 * the interleave set.  Once that value is determined the PMEM-limit for
 * the set can be established.
 */
resource_size_t nd_pmem_available_dpa(struct nd_region *nd_region,
		struct nd_mapping *nd_mapping, resource_size_t *overlap)
{
	resource_size_t map_start, map_end, busy = 0, available, blk_start;
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	struct resource *res;
	const char *reason;

	if (!ndd)
		return 0;

	map_start = nd_mapping->start;
	map_end = map_start + nd_mapping->size - 1;
	blk_start = max(map_start, map_end + 1 - *overlap);
	for_each_dpa_resource(ndd, res) {
		if (res->start >= map_start && res->start < map_end) {
			if (strncmp(res->name, "blk", 3) == 0)
				blk_start = min(blk_start,
						max(map_start, res->start));
			else if (res->end > map_end) {
				reason = "misaligned to iset";
				goto err;
			} else
				busy += resource_size(res);
		} else if (res->end >= map_start && res->end <= map_end) {
			if (strncmp(res->name, "blk", 3) == 0) {
				/*
				 * If a BLK allocation overlaps the start of
				 * PMEM the entire interleave set may now only
				 * be used for BLK.
				 */
				blk_start = map_start;
			} else
				busy += resource_size(res);
		} else if (map_start > res->start && map_start < res->end) {
			/* total eclipse of the mapping */
			busy += nd_mapping->size;
			blk_start = map_start;
		}
	}

	*overlap = map_end + 1 - blk_start;
	available = blk_start - map_start;
	if (busy < available)
		return available - busy;
	return 0;

 err:
	nd_dbg_dpa(nd_region, ndd, res, "%s\n", reason);
	return 0;
}

void nvdimm_free_dpa(struct nvdimm_drvdata *ndd, struct resource *res)
{
	WARN_ON_ONCE(!is_nvdimm_bus_locked(ndd->dev));
	kfree(res->name);
	__release_region(&ndd->dpa, res->start, resource_size(res));
}

struct resource *nvdimm_allocate_dpa(struct nvdimm_drvdata *ndd,
		struct nd_label_id *label_id, resource_size_t start,
		resource_size_t n)
{
	char *name = kmemdup(label_id, sizeof(*label_id), GFP_KERNEL);
	struct resource *res;

	if (!name)
		return NULL;

	WARN_ON_ONCE(!is_nvdimm_bus_locked(ndd->dev));
	res = __request_region(&ndd->dpa, start, n, name, 0);
	if (!res)
		kfree(name);
	return res;
}

/**
 * nvdimm_allocated_dpa - sum up the dpa currently allocated to this label_id
 * @nvdimm: container of dpa-resource-root + labels
 * @label_id: dpa resource name of the form {pmem|blk}-<human readable uuid>
 */
resource_size_t nvdimm_allocated_dpa(struct nvdimm_drvdata *ndd,
		struct nd_label_id *label_id)
{
	resource_size_t allocated = 0;
	struct resource *res;

	for_each_dpa_resource(ndd, res)
		if (strcmp(res->name, label_id->id) == 0)
			allocated += resource_size(res);

	return allocated;
}

static int count_dimms(struct device *dev, void *c)
{
	int *count = c;

	if (is_nvdimm(dev))
		(*count)++;
	return 0;
}

int nvdimm_bus_check_dimm_count(struct nvdimm_bus *nvdimm_bus, int dimm_count)
{
	int count = 0;
	/* Flush any possible dimm registration failures */
	nd_synchronize();

	device_for_each_child(&nvdimm_bus->dev, &count, count_dimms);
	dev_dbg(&nvdimm_bus->dev, "count: %d\n", count);
	if (count != dimm_count)
		return -ENXIO;
	return 0;
}
EXPORT_SYMBOL_GPL(nvdimm_bus_check_dimm_count);

void __exit nvdimm_devs_exit(void)
{
	ida_destroy(&dimm_ida);
}

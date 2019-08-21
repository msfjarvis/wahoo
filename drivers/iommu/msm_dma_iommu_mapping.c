// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2016, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#include <linux/dma-buf.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <asm/barrier.h>

struct msm_iommu_meta {
	struct list_head map_list;
	int refcount;
};

struct msm_iommu_map {
	struct device *dev;
	struct list_head lnode;
	struct scatterlist sgl;
	enum dma_data_direction dir;
	unsigned int nents;
	int refcount;
};

static struct msm_iommu_map *msm_iommu_map_lookup(struct msm_iommu_meta *meta,
						  struct device *dev)
{
	struct msm_iommu_map *map;

	list_for_each_entry(map, &meta->map_list, lnode) {
		if (map->dev == dev)
			return map;
	}

	return NULL;
}

static void msm_iommu_meta_put(struct msm_iommu_data *data)
{
	struct msm_iommu_meta *meta = data->meta;
	struct msm_iommu_map *map, *tmp_map;

	if (--meta->refcount)
		return;

	list_for_each_entry_safe(map, tmp_map, &meta->map_list, lnode) {
		dma_unmap_sg(map->dev, &map->sgl, map->nents, map->dir);
		kfree(map);
	}

	data->meta = NULL;
	kfree(meta);
}

int msm_dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
			 enum dma_data_direction dir, struct dma_buf *dma_buf,
			 struct dma_attrs *attrs)
{
	static const gfp_t gfp_flags_nofail = GFP_KERNEL | __GFP_NOFAIL;
	int not_lazy = dma_get_attr(DMA_ATTR_NO_DELAYED_UNMAP, attrs);
	struct msm_iommu_data *data = dma_buf->priv;
	struct msm_iommu_meta *meta;
	struct msm_iommu_map *map;

	mutex_lock(&data->lock);
	meta = data->meta;
	map = meta ? msm_iommu_map_lookup(meta, dev) : NULL;
	if (map) {
		map->refcount++;
		sg->dma_address = map->sgl.dma_address;
		sg->dma_length = map->sgl.dma_length;
		if (is_device_dma_coherent(dev))
			dmb(ish);
	} else {
		nents = dma_map_sg_attrs(dev, sg, nents, dir, attrs);
		if (nents) {
			map = kmalloc(sizeof(*map), gfp_flags_nofail);
			map->dev = dev;
			map->dir = dir;
			map->nents = nents;
			map->refcount = 2 - not_lazy;
			map->sgl.dma_address = sg->dma_address;
			map->sgl.dma_length = sg->dma_length;

			if (meta) {
				meta->refcount++;
			} else {
				meta = kmalloc(sizeof(*meta), gfp_flags_nofail);
				meta->refcount = 1;
				INIT_LIST_HEAD(&meta->map_list);
				data->meta = meta;
			}
			list_add(&map->lnode, &meta->map_list);
		}
	}
	mutex_unlock(&data->lock);

	return nents;
}

void msm_dma_unmap_sg(struct device *dev, struct scatterlist *sgl, int nents,
		      enum dma_data_direction dir, struct dma_buf *dma_buf)
{
	struct msm_iommu_data *data = dma_buf->priv;
	struct msm_iommu_meta *meta;
	struct msm_iommu_map *map;

	mutex_lock(&data->lock);
	meta = data->meta;
	if (meta) {
		map = msm_iommu_map_lookup(meta, dev);
		if (map && !--map->refcount) {
			dma_unmap_sg(map->dev, &map->sgl, map->nents, map->dir);
			list_del(&map->lnode);
			kfree(map);
			msm_iommu_meta_put(data);
		}
	}
	mutex_unlock(&data->lock);
}

void msm_dma_buf_freed(struct msm_iommu_data *data)
{
	mutex_lock(&data->lock);
	if (data->meta)
		msm_iommu_meta_put(data);
	mutex_unlock(&data->lock);
}

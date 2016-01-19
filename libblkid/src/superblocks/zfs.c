/*
 * Copyright (C) 2009-2010 by Andreas Dilger <adilger@sun.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>

#include "superblocks.h"

#define VDEV_LABEL_UBERBLOCK	(128 * 1024ULL)
#define VDEV_LABEL_NVPAIR	( 16 * 1024ULL)
#define VDEV_LABEL_SIZE		(256 * 1024ULL)
#define UBERBLOCK_SIZE		1024ULL
#define UBERBLOCKS_COUNT   128

/* #include <sys/uberblock_impl.h> */
#define UBERBLOCK_MAGIC         0x00bab10c              /* oo-ba-bloc!  */
struct zfs_uberblock {
	uint64_t	ub_magic;	/* UBERBLOCK_MAGIC		*/
	uint64_t	ub_version;	/* SPA_VERSION			*/
	uint64_t	ub_txg;		/* txg of last sync		*/
	uint64_t	ub_guid_sum;	/* sum of all vdev guids	*/
	uint64_t	ub_timestamp;	/* UTC time of last sync	*/
	char		ub_rootbp;	/* MOS objset_phys_t		*/
} __attribute__((packed));

#define ZFS_WANT	 4

#define DATA_TYPE_UINT64 8
#define DATA_TYPE_STRING 9

struct nvpair {
	uint32_t	nvp_size;
	uint32_t	nvp_unkown;
	uint32_t	nvp_namelen;
	char		nvp_name[0]; /* aligned to 4 bytes */
	/* aligned ptr array for string arrays */
	/* aligned array of data for value */
};

struct nvstring {
	uint32_t	nvs_type;
	uint32_t	nvs_elem;
	uint32_t	nvs_strlen;
	unsigned char	nvs_string[0];
};

struct nvuint64 {
	uint32_t	nvu_type;
	uint32_t	nvu_elem;
	uint64_t	nvu_value;
};

struct nvlist {
	uint32_t	nvl_unknown[3];
	struct nvpair	nvl_nvpair;
};

#define nvdebug(fmt, ...)	do { } while(0)
/*#define nvdebug(fmt, a...)	fprintf(stderr, fmt, ##a)*/

static void zfs_extract_guid_name(blkid_probe pr, loff_t offset)
{
	unsigned char *p, buff[4096];
	struct nvlist *nvl;
	struct nvpair *nvp;
	size_t left = sizeof(buff);
	int found = 0;

	offset = (offset & ~(VDEV_LABEL_SIZE - 1)) + VDEV_LABEL_NVPAIR;

	/* Note that we currently assume that the desired fields are within
	 * the first 4k (left) of the nvlist.  This is true for all pools
	 * I've seen, and simplifies this code somewhat, because we don't
	 * have to handle an nvpair crossing a buffer boundary. */
	p = blkid_probe_get_buffer(pr, offset, left);
	if (!p)
		return;

	/* libblkid buffers are strictly readonly, but the code below modifies nvpair etc. */
	memcpy(buff, p, sizeof(buff));
	nvl = (struct nvlist *) buff;

	nvdebug("zfs_extract: nvlist offset %llu\n", offset);

	nvp = &nvl->nvl_nvpair;
	while (left > sizeof(*nvp) && nvp->nvp_size != 0 && found < 3) {
		int avail;   /* tracks that name/value data fits in nvp_size */
		int namesize;

		nvp->nvp_size = be32_to_cpu(nvp->nvp_size);
		nvp->nvp_namelen = be32_to_cpu(nvp->nvp_namelen);
		avail = nvp->nvp_size - nvp->nvp_namelen - sizeof(*nvp);

		nvdebug("left %zd nvp_size %u\n", left, nvp->nvp_size);
		if (left < nvp->nvp_size || avail < 0)
			break;

		namesize = (nvp->nvp_namelen + 3) & ~3;

		nvdebug("nvlist: size %u, namelen %u, name %*s\n",
			nvp->nvp_size, nvp->nvp_namelen, nvp->nvp_namelen,
			nvp->nvp_name);
		if (strncmp(nvp->nvp_name, "name", nvp->nvp_namelen) == 0) {
			struct nvstring *nvs = (void *)(nvp->nvp_name+namesize);

			nvs->nvs_type = be32_to_cpu(nvs->nvs_type);
			nvs->nvs_strlen = be32_to_cpu(nvs->nvs_strlen);
			if (nvs->nvs_strlen > UINT_MAX - sizeof(*nvs))
				break;
			avail -= nvs->nvs_strlen + sizeof(*nvs);
			nvdebug("nvstring: type %u string %*s\n", nvs->nvs_type,
				nvs->nvs_strlen, nvs->nvs_string);
			if (nvs->nvs_type == DATA_TYPE_STRING && avail >= 0)
				blkid_probe_set_label(pr, nvs->nvs_string,
						      nvs->nvs_strlen);
			found++;
		} else if (strncmp(nvp->nvp_name, "guid",
				   nvp->nvp_namelen) == 0) {
			struct nvuint64 *nvu = (void *)(nvp->nvp_name+namesize);
			uint64_t nvu_value;

			memcpy(&nvu_value, &nvu->nvu_value, sizeof(nvu_value));
			nvu->nvu_type = be32_to_cpu(nvu->nvu_type);
			nvu_value = be64_to_cpu(nvu_value);
			avail -= sizeof(*nvu);
			nvdebug("nvuint64: type %u value %"PRIu64"\n",
				nvu->nvu_type, nvu_value);
			if (nvu->nvu_type == DATA_TYPE_UINT64 && avail >= 0)
				blkid_probe_sprintf_value(pr, "UUID_SUB",
							  "%"PRIu64, nvu_value);
			found++;
		} else if (strncmp(nvp->nvp_name, "pool_guid",
				   nvp->nvp_namelen) == 0) {
			struct nvuint64 *nvu = (void *)(nvp->nvp_name+namesize);
			uint64_t nvu_value;

			memcpy(&nvu_value, &nvu->nvu_value, sizeof(nvu_value));
			nvu->nvu_type = be32_to_cpu(nvu->nvu_type);
			nvu_value = be64_to_cpu(nvu_value);
			avail -= sizeof(*nvu);
			nvdebug("nvuint64: type %u value %"PRIu64"\n",
				nvu->nvu_type, nvu_value);
			if (nvu->nvu_type == DATA_TYPE_UINT64 && avail >= 0)
				blkid_probe_sprintf_uuid(pr, (unsigned char *)
							 &nvu_value,
							 sizeof(nvu_value),
							 "%"PRIu64, nvu_value);
			found++;
		}
		if (left > nvp->nvp_size)
			left -= nvp->nvp_size;
		else
			left = 0;
		nvp = (struct nvpair *)((char *)nvp + nvp->nvp_size);
	}
}

#define zdebug(fmt, ...)	do {} while(0)
/*#define zdebug(fmt, a...)	fprintf(stderr, fmt, ##a)*/

static int find_uberblocks(const void *label, loff_t *ub_offset, int *swap_endian)
{
	uint64_t swab_magic = swab64((uint64_t)UBERBLOCK_MAGIC);
	struct zfs_uberblock *ub;
	int i, found = 0;
	loff_t offset = VDEV_LABEL_UBERBLOCK;

	for (i = 0; i < UBERBLOCKS_COUNT; i++, offset += UBERBLOCK_SIZE) {
		ub = (struct zfs_uberblock *)(label + offset);

		if (ub->ub_magic == UBERBLOCK_MAGIC) {
			*ub_offset = offset;
			*swap_endian = 0;
			found++;
			zdebug("probe_zfs: found little-endian uberblock at %llu\n", offset >> 10);
		}

		if (ub->ub_magic == swab_magic) {
			*ub_offset = offset;
			*swap_endian = 1;
			found++;
			zdebug("probe_zfs: found big-endian uberblock at %llu\n", offset >> 10);
		}
	}

	return found;
}

/* ZFS has 128x1kB host-endian root blocks, stored in 2 areas at the start
 * of the disk, and 2 areas at the end of the disk.  Check only some of them...
 * #4 (@ 132kB) is the first one written on a new filesystem. */
static int probe_zfs(blkid_probe pr, const struct blkid_idmag *mag)
{
	int swab_endian = 0;
	struct zfs_uberblock *ub;
	loff_t offset, ub_offset = 0;
	int label_no, found = 0, found_in_label;
	void *label;
	loff_t blk_align = (pr->size % (256 * 1024ULL));

	zdebug("probe_zfs\n");
	/* Look for at least 4 uberblocks to ensure a positive match */
	for (label_no = 0; label_no < 4; label_no++) {
		switch(label_no) {
		case 0: // jump to L0
			offset = 0;
			break;
		case 1: // jump to L1
			offset = VDEV_LABEL_SIZE;
			break;
		case 2: // jump to L2
			offset = pr->size - 2 * VDEV_LABEL_SIZE - blk_align;
			break;
		case 3: // jump to L3
			offset = pr->size - VDEV_LABEL_SIZE - blk_align;
			break;
		}

		label = blkid_probe_get_buffer(pr, offset, VDEV_LABEL_SIZE);
		if (label == NULL)
			return errno ? -errno : 1;

		found_in_label = find_uberblocks(label, &ub_offset, &swab_endian);

		if (found_in_label > 0) {
			found+= found_in_label;
			ub = (struct zfs_uberblock *)(label + ub_offset);
			ub_offset += offset;

			if (found >= ZFS_WANT)
				break;
		}
	}

	if (found < ZFS_WANT)
		return 1;

	/* If we found the 4th uberblock, then we will have exited from the
	 * scanning loop immediately, and ub will be a valid uberblock. */
	blkid_probe_sprintf_version(pr, "%" PRIu64, swab_endian ?
				    swab64(ub->ub_version) : ub->ub_version);

	zfs_extract_guid_name(pr, offset);

	if (blkid_probe_set_magic(pr, ub_offset,
				sizeof(ub->ub_magic),
				(unsigned char *) &ub->ub_magic))
		return 1;

	return 0;
}

const struct blkid_idinfo zfs_idinfo =
{
	.name		= "zfs_member",
	.usage		= BLKID_USAGE_FILESYSTEM,
	.probefunc	= probe_zfs,
	.minsz		= 64 * 1024 * 1024,
	.magics		= BLKID_NONE_MAGIC
};

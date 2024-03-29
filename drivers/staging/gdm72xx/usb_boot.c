/*
 * Copyright (c) 2012 GCT Semiconductor, Inc. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/usb.h>
#include <linux/unistd.h>
#include <linux/slab.h>

#include <asm/byteorder.h>
#include "gdm_usb.h"
#include "usb_boot.h"

#define DN_KERNEL_MAGIC_NUMBER		0x10760001
#define DN_ROOTFS_MAGIC_NUMBER		0x10760002

#define DOWNLOAD_SIZE	1024

#define DH2B(x)		__cpu_to_be32(x)
#define DL2H(x)		__le32_to_cpu(x)

#define MIN(a, b)	((a) > (b) ? (b) : (a))

#define MAX_IMG_CNT		16
#define UIMG_PATH		"/*(DEBLOBBED)*/"
#define KERN_PATH		"/lib/firmware/gdm72xx/zImage"
#define FS_PATH			"/lib/firmware/gdm72xx/ramdisk.jffs2"

struct dn_header {
	u32	magic_num;
	u32	file_size;
};

struct img_header {
	u32		magic_code;
	u32		count;
	u32		len;
	u32		offset[MAX_IMG_CNT];
	char	hostname[32];
	char	date[32];
};

struct fw_info {
	u32		id;
	u32		len;
	u32		kernel_len;
	u32		rootfs_len;
	u32		kernel_offset;
	u32		rootfs_offset;
	u32		fw_ver;
	u32		mac_ver;
	char	hostname[32];
	char	userid[16];
	char	date[32];
	char	user_desc[128];
};

static void array_le32_to_cpu(u32 *arr, int num)
{
	int i;
	for (i = 0; i < num; i++, arr++)
		*arr = DL2H(*arr);
}

static u8 *tx_buf;

static int gdm_wibro_send(struct usb_device *usbdev, void *data, int len)
{
	int ret;
	int actual;

	ret = usb_bulk_msg(usbdev, usb_sndbulkpipe(usbdev, 1), data, len,
			&actual, 1000);

	if (ret < 0) {
		printk(KERN_ERR "Error : usb_bulk_msg ( result = %d )\n", ret);
		return ret;
	}
	return 0;
}

static int gdm_wibro_recv(struct usb_device *usbdev, void *data, int len)
{
	int ret;
	int actual;

	ret = usb_bulk_msg(usbdev, usb_rcvbulkpipe(usbdev, 2), data, len,
			&actual, 5000);

	if (ret < 0) {
		printk(KERN_ERR "Error : usb_bulk_msg(recv) ( result = %d )\n",
			ret);
		return ret;
	}
	return 0;
}

static int download_image(struct usb_device *usbdev, struct file *filp,
				loff_t *pos, u32 img_len, u32 magic_num)
{
	struct dn_header h;
	int ret = 0;
	u32 size;
	int len, readn;

	size = (img_len + DOWNLOAD_SIZE - 1) & ~(DOWNLOAD_SIZE - 1);
	h.magic_num = DH2B(magic_num);
	h.file_size = DH2B(size);

	ret = gdm_wibro_send(usbdev, &h, sizeof(h));
	if (ret < 0)
		goto out;

	readn = 0;
	while ((len = filp->f_op->read(filp, tx_buf, DOWNLOAD_SIZE, pos))) {

		if (len < 0) {
			ret = -1;
			goto out;
		}
		readn += len;

		ret = gdm_wibro_send(usbdev, tx_buf, DOWNLOAD_SIZE);
		if (ret < 0)
			goto out;
		if (readn >= img_len)
			break;
	}

	if (readn < img_len) {
		printk(KERN_ERR "gdmwm: Cannot read to the requested size. "
			"Read = %d Requested = %d\n", readn, img_len);
		ret = -EIO;
	}
out:

	return ret;
}

int usb_boot(struct usb_device *usbdev, u16 pid)
{
	int i, ret = 0;
	struct file *filp = NULL;
	struct inode *inode = NULL;
	static mm_segment_t fs;
	struct img_header hdr;
	struct fw_info fw_info;
	loff_t pos = 0;
	char *img_name = UIMG_PATH;
	int len;

	tx_buf = kmalloc(DOWNLOAD_SIZE, GFP_KERNEL);
	if (tx_buf == NULL) {
		printk(KERN_ERR "Error: kmalloc\n");
		return -ENOMEM;
	}

	fs = get_fs();
	set_fs(get_ds());

	/*(DEBLOBBED)*/
	if (1 /*(DEBLOBBED)*/) {
		printk(KERN_ERR "Can't find %s.\n", img_name);
		set_fs(fs);
		ret = -ENOENT;
		goto restore_fs;
	}

	if (filp->f_dentry)
		inode = filp->f_dentry->d_inode;
	if (!inode || !S_ISREG(inode->i_mode)) {
		printk(KERN_ERR "Invalid file type: %s\n", img_name);
		ret = -EINVAL;
		goto out;
	}

	len = filp->f_op->read(filp, (u8 *)&hdr, sizeof(hdr), &pos);
	if (len != sizeof(hdr)) {
		printk(KERN_ERR "gdmwm: Cannot read the image info.\n");
		ret = -EIO;
		goto out;
	}

	array_le32_to_cpu((u32 *)&hdr, 19);
#if 0
	if (hdr.magic_code != 0x10767fff) {
		printk(KERN_ERR "gdmwm: Invalid magic code 0x%08x\n",
			hdr.magic_code);
		ret = -EINVAL;
		goto out;
	}
#endif
	if (hdr.count > MAX_IMG_CNT) {
		printk(KERN_ERR "gdmwm: Too many images. %d\n", hdr.count);
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < hdr.count; i++) {
		if (hdr.offset[i] > hdr.len) {
			printk(KERN_ERR "gdmwm: Invalid offset. "
				"Entry = %d Offset = 0x%08x "
				"Image length = 0x%08x\n",
				i, hdr.offset[i], hdr.len);
			ret = -EINVAL;
			goto out;
		}

		pos = hdr.offset[i];
		len = filp->f_op->read(filp, (u8 *)&fw_info, sizeof(fw_info),
					&pos);
		if (len != sizeof(fw_info)) {
			printk(KERN_ERR "gdmwm: Cannot read the FW info.\n");
			ret = -EIO;
			goto out;
		}

		array_le32_to_cpu((u32 *)&fw_info, 8);
#if 0
		if ((fw_info.id & 0xfffff000) != 0x10767000) {
			printk(KERN_ERR "gdmwm: Invalid FW id. 0x%08x\n",
				fw_info.id);
			ret = -EIO;
			goto out;
		}
#endif

		if ((fw_info.id & 0xffff) != pid)
			continue;

		pos = hdr.offset[i] + fw_info.kernel_offset;
		ret = download_image(usbdev, filp, &pos, fw_info.kernel_len,
				DN_KERNEL_MAGIC_NUMBER);
		if (ret < 0)
			goto out;
		printk(KERN_INFO "GCT: Kernel download success.\n");

		pos = hdr.offset[i] + fw_info.rootfs_offset;
		ret = download_image(usbdev, filp, &pos, fw_info.rootfs_len,
				DN_ROOTFS_MAGIC_NUMBER);
		if (ret < 0)
			goto out;
		printk(KERN_INFO "GCT: Filesystem download success.\n");

		break;
	}

	if (i == hdr.count) {
		printk(KERN_ERR "Firmware for gsk%x is not installed.\n", pid);
		ret = -EINVAL;
	}
out:
	filp_close(filp, current->files);

restore_fs:
	set_fs(fs);
	kfree(tx_buf);
	return ret;
}

/*#define GDM7205_PADDING		256 */
#define DOWNLOAD_CHUCK			2048
#define KERNEL_TYPE_STRING		"linux"
#define FS_TYPE_STRING			"rootfs"

static int em_wait_ack(struct usb_device *usbdev, int send_zlp)
{
	int ack;
	int ret = -1;

	if (send_zlp) {
		/*Send ZLP*/
		ret = gdm_wibro_send(usbdev, NULL, 0);
		if (ret < 0)
			goto out;
	}

	/*Wait for ACK*/
	ret = gdm_wibro_recv(usbdev, &ack, sizeof(ack));
	if (ret < 0)
		goto out;
out:
	return ret;
}

static int em_download_image(struct usb_device *usbdev, char *path,
				char *type_string)
{
	struct file *filp;
	struct inode *inode;
	static mm_segment_t fs;
	char *buf = NULL;
	loff_t pos = 0;
	int ret = 0;
	int len, readn = 0;
	#if defined(GDM7205_PADDING)
	const int pad_size = GDM7205_PADDING;
	#else
	const int pad_size = 0;
	#endif

	fs = get_fs();
	set_fs(get_ds());

	filp = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
	if (IS_ERR(filp)) {
		printk(KERN_ERR "Can't find %s.\n", path);
		set_fs(fs);
		ret = -ENOENT;
		goto restore_fs;
	}

	if (filp->f_dentry) {
		inode = filp->f_dentry->d_inode;
		if (!inode || !S_ISREG(inode->i_mode)) {
			printk(KERN_ERR "Invalid file type: %s\n", path);
			ret = -EINVAL;
			goto out;
		}
	}

	buf = kmalloc(DOWNLOAD_CHUCK + pad_size, GFP_KERNEL);
	if (buf == NULL) {
		printk(KERN_ERR "Error: kmalloc\n");
		return -ENOMEM;
	}

	strcpy(buf+pad_size, type_string);
	ret = gdm_wibro_send(usbdev, buf, strlen(type_string)+pad_size);
	if (ret < 0)
		goto out;

	while ((len = filp->f_op->read(filp, buf+pad_size, DOWNLOAD_CHUCK,
					&pos))) {
		if (len < 0) {
			ret = -1;
			goto out;
		}
		readn += len;

		ret = gdm_wibro_send(usbdev, buf, len+pad_size);
		if (ret < 0)
			goto out;

		ret = em_wait_ack(usbdev, ((len+pad_size) % 512 == 0));
		if (ret < 0)
			goto out;
	}

	ret = em_wait_ack(usbdev, 1);
	if (ret < 0)
		goto out;

out:
	filp_close(filp, current->files);

restore_fs:
	set_fs(fs);

	kfree(buf);

	return ret;
}

static int em_fw_reset(struct usb_device *usbdev)
{
	int ret;

	/*Send ZLP*/
	ret = gdm_wibro_send(usbdev, NULL, 0);
	return ret;
}

int usb_emergency(struct usb_device *usbdev)
{
	int ret;

	ret = em_download_image(usbdev, KERN_PATH, KERNEL_TYPE_STRING);
	if (ret < 0)
		goto out;
	printk(KERN_INFO "GCT Emergency: Kernel download success.\n");

	ret = em_download_image(usbdev, FS_PATH, FS_TYPE_STRING);
	if (ret < 0)
		goto out;
	printk(KERN_INFO "GCT Emergency: Filesystem download success.\n");

	ret = em_fw_reset(usbdev);
out:
	return ret;
}

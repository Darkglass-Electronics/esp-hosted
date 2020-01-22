#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "esp.h"
#include "esp_rb.h"
#include "esp_api.h"

#define ESP_SERIAL_MAJOR	221
#define ESP_SERIAL_MINOR_MAX	2
#define ESP_RX_RB_SIZE	4096

//#define ESP_SERIAL_TEST


static struct esp_serial_devs {
	struct cdev cdev;
	int dev_index;
	esp_rb_t rb;
	void *priv;
} devs[ESP_SERIAL_MINOR_MAX];

static int esp_serial_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset)
{
	struct esp_serial_devs *dev;
       	dev = (struct esp_serial_devs *) file->private_data;
	size = esp_rb_read_by_user(&dev->rb, user_buffer, size, file->f_flags & O_NONBLOCK);
/*	print_hex_dump_bytes("Rx:", DUMP_PREFIX_NONE, user_buffer, size);*/
	if (size == 0) {
		return -EAGAIN;
	}

	return size;
}

static int esp_serial_write(struct file *file, const char __user *user_buffer, size_t size, loff_t * offset)
{
	struct esp32_payload_header *hdr;
	char *buf;
	struct esp_serial_devs *dev;
	int ret;
	size_t total_len;

       	dev = (struct esp_serial_devs *) file->private_data;
	total_len = size + sizeof(struct esp32_payload_header);

	buf = kmalloc(total_len, GFP_KERNEL);
	if (!buf) {
		printk(KERN_ERR "Error allocating buffer to send serial data\n");
		return -ENOMEM;
	}

	hdr = (struct esp32_payload_header *) buf;

	hdr->if_type = ESP_IF_SERIAL;
	hdr->if_num = dev->dev_index;
	hdr->len = size;
	hdr->offset = sizeof(struct esp32_payload_header);
	
	ret = copy_from_user(buf + hdr->offset, user_buffer, size);
	if (ret != 0) {
		kfree(buf);
		printk(KERN_ERR "Error copying buffer to send serial data\n");
		return -EFAULT;
	}

	print_hex_dump_bytes("Tx:", DUMP_PREFIX_NONE, buf, total_len);
	ret = esp32_send_packet(dev->priv, buf, total_len);
	if (ret) {
		printk (KERN_ERR "%s: Failed to transmit data\n", __func__);
		/* TODO: Stop the datapath if error count exceeds max count*/
	}
	
	kfree(buf);
	return size;
}

static long esp_serial_ioctl (struct file *file, unsigned int cmd, unsigned long arg)
{
	printk(KERN_ERR "%s IOCTL %d\n", __func__, cmd);
	return 0;
}

static int esp_serial_open(struct inode *inode, struct file *file)
{
	struct esp_serial_devs *devs;

	devs = container_of(inode->i_cdev, struct esp_serial_devs, cdev);
	file->private_data = devs;
	printk(KERN_ERR "%s on device %d\n", __func__, devs->dev_index);

	return 0;
}

const struct file_operations esp_serial_fops = {
	.owner = THIS_MODULE,
	.open = esp_serial_open,
	.read = esp_serial_read,
	.write = esp_serial_write,
	.unlocked_ioctl = esp_serial_ioctl
};

int esp_serial_data_received(int dev_index, const char *data, size_t len)
{
	int ret;
	size_t ret_len = 0;

	while (ret_len != len) { 
		ret = esp_rb_write_by_kernel(&devs[dev_index].rb, data, len);
		ret_len += ret;
		if (ret == 0) {
			break;
		}
	}
	if (ret_len != len) {
		printk(KERN_ERR "RB full, no space to receive. Dropping packet");
	}

	return ret_len;
}

#ifdef ESP_SERIAL_TEST
static int thread_fn(void *unused)
{
	int i = 100;

	while(i--) {
		esp_rb_write_by_kernel(&devs[0].rb, "alphabetagamma", 14);
		ssleep(1);
	}
	printk(KERN_INFO "Thread stopping\n");
	do_exit(0);
	return 0;
}
#endif

int esp_serial_init(void *priv)
{
	int err;
	int i;

	err = register_chrdev_region(MKDEV(ESP_SERIAL_MAJOR, 0), ESP_SERIAL_MINOR_MAX, "esp_serial_driver");
	if (err) {
		printk(KERN_ERR "Error registering chrdev region %d\n", err);
		return -1;
	}

	for (i = 0; i < ESP_SERIAL_MINOR_MAX; i++) { 
		cdev_init(&devs[i].cdev, &esp_serial_fops);
		devs[i].dev_index = i;
		cdev_add(&devs[i].cdev, MKDEV(ESP_SERIAL_MAJOR, i), 1);
		esp_rb_init(&devs[i].rb, ESP_RX_RB_SIZE);
		devs[i].priv = priv;
	}

#ifdef ESP_SERIAL_TEST	
	kthread_run(thread_fn, NULL, "esptest-thread");
#endif
	return 0;	
}

void esp_serial_cleanup(void)
{
	int i;
	for (i = 0; i < ESP_SERIAL_MINOR_MAX; i++) {
		cdev_del(&devs[i].cdev);
	}
	unregister_chrdev_region(MKDEV(ESP_SERIAL_MAJOR, 0), ESP_SERIAL_MINOR_MAX);
	return;
}

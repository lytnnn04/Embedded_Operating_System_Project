#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/err.h>

#define GPIO1_BASE          0x4804C000
#define GPIO1_SIZE          0x1000
#define GPIO_OE             0x134
#define GPIO_DATAOUT        0x13C
#define GPIO_SETDATAOUT     0x194
#define GPIO_CLEARDATAOUT   0x190
#define LED_PIN_BIT         (1 << 28)  // GPIO1_28 - P9 pin 12

#define DEVICE_NAME         "led"
#define CLASS_NAME          "led_class"

static int major;
static struct class *led_class = NULL;
static struct device *led_device = NULL;
void __iomem *v_base;

static int led_open(struct inode *inode, struct file *file) {
    return 0;
}

static int led_release(struct inode *inode, struct file *file) {
    return 0;
}

static ssize_t led_read(struct file *file, char __user *buffer, size_t len, loff_t *off) {
    char msg[2];
    uint32_t reg_val;

    if (*off > 0) return 0;

    reg_val = ioread32(v_base + GPIO_DATAOUT);
    msg[0] = (reg_val & LED_PIN_BIT) ? '1' : '0';
    msg[1] = '\n';

    if (copy_to_user(buffer, msg, 2)) return -EFAULT;

    *off += 2;
    return 2;
}

static ssize_t led_write(struct file *file, const char __user *buffer, size_t len, loff_t *off) {
    char kbuf;
    if (copy_from_user(&kbuf, buffer, 1)) return -EFAULT;

    if (kbuf == '1') {
        iowrite32(LED_PIN_BIT, v_base + GPIO_SETDATAOUT);
        printk(KERN_INFO "Nam_PTIT_Driver: Da BAT LED USR3\n");
    }
    else if (kbuf == '0') {
        iowrite32(LED_PIN_BIT, v_base + GPIO_CLEARDATAOUT);
        printk(KERN_INFO "Nam_PTIT_Driver: Da TAT LED USR3\n");
    }
    return len;
}

static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = led_open,
    .release = led_release,
    .read    = led_read,
    .write   = led_write,
};

static int __init led_driver_init(void) {
    uint32_t reg_val;

    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) return major;

    led_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(led_class)) {
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(led_class);
    }

    led_device = device_create(led_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(led_device)) {
        class_destroy(led_class);
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(led_device);
    }

    v_base = ioremap(GPIO1_BASE, GPIO1_SIZE);
    if (!v_base) goto r_device;

    reg_val = ioread32(v_base + GPIO_OE);
    reg_val &= ~LED_PIN_BIT;
    iowrite32(reg_val, v_base + GPIO_OE);

    printk(KERN_INFO "Nam_PTIT_Driver: Nap thanh cong!\n");
    return 0;

r_device:
    device_destroy(led_class, MKDEV(major, 0));
    class_destroy(led_class);
    unregister_chrdev(major, DEVICE_NAME);
    return -ENOMEM;
}

static void __exit led_driver_exit(void) {
    iowrite32(LED_PIN_BIT, v_base + GPIO_CLEARDATAOUT);
    iounmap(v_base);
    device_destroy(led_class, MKDEV(major, 0));
    class_destroy(led_class);
    unregister_chrdev(major, DEVICE_NAME);
    printk(KERN_INFO "Nam_PTIT_Driver: Go cai dat thanh cong.\n");
}

module_init(led_driver_init);
module_exit(led_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nam_PTIT");

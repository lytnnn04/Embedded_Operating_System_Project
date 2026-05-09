#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/delay.h>

#define DRIVER_NAME "adc_register_driver"
#define CLASS_NAME  "adc_class"
#define DEVICE_NAME "adc"

#define CM_WKUP_BASE        0x44E00400
#define ADC_BASE            0x44E0D000

static dev_t dev_num;
static struct cdev my_cdev;
static struct class *my_class;
static struct device *my_device;

void __iomem *cm_wkup_addr;
void __iomem *adc_base_addr;

static ssize_t my_read(struct file *file, char __user *user_buf, size_t len, loff_t *offset) {
    uint32_t count, data;
    char buf[16];
    int bytes_read;

    if (*offset > 0) return 0;

    iowrite32((1 << 1), adc_base_addr + 0x54); // Start Step 1
    
    count = 0;
    while ((ioread32(adc_base_addr + 0xE4) == 0) && (count < 100)) {
        udelay(10);
        count++;
    }

    if (count >= 100) return -EFAULT;

    data = ioread32(adc_base_addr + 0x100) & 0xFFF; // Doc FIFO0
    bytes_read = snprintf(buf, sizeof(buf), "%u\n", data);
    
    if (copy_to_user(user_buf, buf, bytes_read)) return -EFAULT;
    
    *offset += bytes_read;
    return bytes_read;
}

static int my_open(struct inode *inode, struct file *file) { return 0; }
static int my_release(struct inode *inode, struct file *file) { return 0; }

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = my_open,
    .release = my_release,
    .read = my_read,
};

static int __init my_driver_init(void) {
    uint32_t reg;
    
    printk(KERN_INFO "[ADC_DEBUG] 1. Bat dau Init Driver...\n");

    cm_wkup_addr = ioremap(CM_WKUP_BASE, 0x1000);
    adc_base_addr = ioremap(ADC_BASE, 0x1000);
    
    if (!cm_wkup_addr || !adc_base_addr) {
        printk(KERN_ERR "[ADC_DEBUG] LOI: ioremap that bai!\n");
        return -ENOMEM;
    }
    printk(KERN_INFO "[ADC_DEBUG] 2. ioremap thanh cong.\n");

    // Bat Clock
    iowrite32(0x02, cm_wkup_addr + 0xBC); 
    printk(KERN_INFO "[ADC_DEBUG] 3. Da ghi lenh bat Clock (CM_WKUP_ADC_CLKCTRL).\n");
    
    // DELAY QUAN TRỌNG: Chờ một chút để tín hiệu Clock thực sự lan tỏa đến module ADC
    // Nếu đọc/ghi adc_base_addr ngay lập tức ở đây, 99% sẽ treo CPU
    msleep(10); 
    printk(KERN_INFO "[ADC_DEBUG] 4. Clock on dinh. Bat dau cau hinh ADC...\n");

    reg = ioread32(adc_base_addr + 0x40);
    reg |= (1 << 2); 
    iowrite32(reg, adc_base_addr + 0x40); // Mo khoa bao ve
    printk(KERN_INFO "[ADC_DEBUG] 5. Da mo khoa StepConfig Write Protect.\n");
    
    iowrite32(0x00000000, adc_base_addr + 0x64); // Cau hinh Step 1 (AIN0)
    iowrite32(0x0000000F, adc_base_addr + 0x68); // Delay
    
    reg = ioread32(adc_base_addr + 0x40);
    reg |= (1 << 0);
    iowrite32(reg, adc_base_addr + 0x40); // Bat Module
    printk(KERN_INFO "[ADC_DEBUG] 6. Da bat Module ADC (CTRL Register).\n");

    // Dang ky Character Device
    alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    cdev_init(&my_cdev, &fops);
    cdev_add(&my_cdev, dev_num, 1);
    my_class = class_create(THIS_MODULE, CLASS_NAME);
    my_device = device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME);

    printk(KERN_INFO "[ADC_DEBUG] 7. Init hoan tat! Device san sang tai /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit my_driver_exit(void) {
    uint32_t reg;
    printk(KERN_INFO "[ADC_DEBUG] Dang xoa Driver...\n");
    
    reg = ioread32(adc_base_addr + 0x40);
    reg &= ~(1 << 0);
    iowrite32(reg, adc_base_addr + 0x40);

    iounmap(cm_wkup_addr);
    iounmap(adc_base_addr);

    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);
    
    printk(KERN_INFO "[ADC_DEBUG] Da xoa Driver an toan.\n");
}

module_init(my_driver_init);
module_exit(my_driver_exit);
MODULE_LICENSE("GPL");

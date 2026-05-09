#include <linux/module.h>       /* module_init, module_exit, MODULE_* */
#include <linux/kernel.h>       /* printk, pr_info, pr_err            */
#include <linux/init.h>         /* __init, __exit                      */
#include <linux/fs.h>           /* file_operations, register_chrdev    */
#include <linux/cdev.h>         /* cdev_init, cdev_add, cdev_del       */
#include <linux/device.h>       /* class_create, device_create         */
#include <linux/uaccess.h>      /* copy_to_user, copy_from_user        */
#include <linux/io.h>           /* ioremap, iounmap, readl, writel     */
#include <linux/delay.h>        /* udelay, mdelay, msleep              */
#include <linux/mutex.h>        /* mutex_lock, mutex_unlock             */
#include <linux/slab.h>         /* kzalloc, kfree                      */
#include <linux/jiffies.h>      /* jiffies, time_before, msecs_to_jiffies */
#include <linux/sched/signal.h> /* signal_pending, current                */

/* ===========================================================================
 * DINH NGHIA HANG SO - AM335x GPIO
 * =========================================================================== */

#define DRIVER_NAME     "dht22_driver"
#define CLASS_NAME      "dht22_class"
#define DEVICE_NAME     "dht22"

/*
 * Dia chi co so (Physical Base Address) cua 4 module GPIO tren AM335x
 * Tham khao: AM335x Technical Reference Manual (TRM), Chapter 25
 */
#define GPIO0_BASE      0x44E07000
#define GPIO1_BASE      0x4804C000
#define GPIO2_BASE      0x481AC000
#define GPIO3_BASE      0x481AE000

#define GPIO_REG_SIZE   0x1A0   /* Kich thuoc vung thanh ghi moi module GPIO */
#define GPIO_NUM_MODULES 4      /* So module GPIO tren AM335x               */

/*
 * OFFSET cac thanh ghi GPIO cua AM335x (tinh tu module base)
 * Tham khao: AM335x TRM, Table 25-5
 *
 * GPIO_OE            : Output Enable (0=output, 1=input)
 * GPIO_DATAIN        : Doc muc logic chan GPIO
 * GPIO_DATAOUT       : Gia tri output hien tai
 * GPIO_SETDATAOUT    : Ghi 1 de SET chan len HIGH
 * GPIO_CLEARDATAOUT  : Ghi 1 de CLEAR chan xuong LOW
 */
#define GPIO_OE              0x134
#define GPIO_DATAIN          0x138
#define GPIO_DATAOUT         0x13C
#define GPIO_CLEARDATAOUT    0x190
#define GPIO_SETDATAOUT      0x194

/*
 * GPIO mac dinh: GPIO1_16 = pin 48 = Header P9_15
 */
#define DEFAULT_GPIO_NUM    48  /* P9_15 = GPIO1_16 */

/* Timeout cho giao thuc DHT22 (microseconds) */
#define DHT22_TIMEOUT_US    200

/* Chu ky doc du lieu DHT22 (milliseconds) - 5 giay */
#define DHT22_READ_INTERVAL_MS  5000

/* So byte du lieu DHT22 tra ve */
#define DHT22_DATA_BYTES    5
#define DHT22_DATA_BITS     40

/* ===========================================================================
 * CAU TRUC DU LIEU DRIVER
 * =========================================================================== */

/* Bang dia chi co so cua 4 module GPIO */
static const unsigned long gpio_module_base[GPIO_NUM_MODULES] = {
    GPIO0_BASE,     /* Module 0: GPIO 0-31  */
    GPIO1_BASE,     /* Module 1: GPIO 32-63 */
    GPIO2_BASE,     /* Module 2: GPIO 64-95 */
    GPIO3_BASE,     /* Module 3: GPIO 96-127 */
};

struct dht22_dev {
    dev_t           dev_num;        /* So thiet bi (major:minor)            */
    struct cdev     cdev;           /* Character device                     */
    struct class    *dev_class;     /* Device class trong /sys/class        */
    struct device   *device;        /* Device trong /dev                    */

    /* Anh xa thanh ghi GPIO - moi module duoc ioremap rieng */
    void __iomem    *gpio_regs[GPIO_NUM_MODULES];
    bool            gpio_mapped[GPIO_NUM_MODULES];

    int             gpio_num;       /* So GPIO tong (0-127)                 */
    int             gpio_module;    /* Module GPIO (0-3)                    */
    int             gpio_pin;       /* Pin trong module (0-31)              */

    int             humidity_int;   /* Do am - phan nguyen                  */
    int             humidity_dec;   /* Do am - phan thap phan               */
    int             temp_sign;      /* Nhiet do - dau (am hoac duong)       */
    int             temp_int;       /* Nhiet do - phan nguyen               */
    int             temp_dec;       /* Nhiet do - phan thap phan            */

    struct mutex    lock;           /* Mutex bao ve truy cap dong thoi      */
    bool            is_open;        /* Trang thai thiet bi                  */

    unsigned long   last_read_jiffies;  /* Thoi diem doc lan cuoi (jiffies) */
    bool            has_read;           /* Da doc it nhat 1 lan chua        */
};

/* Bien toan cuc */
static struct dht22_dev *dht22_device;

/* Module parameters - thay doi khi insmod */
static int gpio_num = DEFAULT_GPIO_NUM;
module_param(gpio_num, int, 0644);
MODULE_PARM_DESC(gpio_num, "So GPIO ket noi DHT22 (0-127, mac dinh: 48 = P9_15)");

/* ===========================================================================
 * HAM HO TRO - TINH TOAN MODULE VA PIN
 * =========================================================================== */

/**
 * gpio_calc_module_pin - Tinh module va pin tu so GPIO tong
 */
static inline int gpio_get_module(int num) { return num / 32; }
static inline int gpio_get_pin(int num)    { return num % 32; }

/**
 * gpio_update_pin - Cap nhat thong tin module/pin khi doi GPIO
 */
static void gpio_update_pin(struct dht22_dev *dev, int num)
{
    dev->gpio_num    = num;
    dev->gpio_module = gpio_get_module(num);
    dev->gpio_pin    = gpio_get_pin(num);
}

/* ===========================================================================
 * CAC HAM TRUY CAP TRUC TIEP THANH GHI GPIO AM335x
 * =========================================================================== */

/**
 * gpio_reg_set_output - Cau hinh chan GPIO lam OUTPUT
 */
static void gpio_reg_set_output(struct dht22_dev *dev)
{
    void __iomem *base = dev->gpio_regs[dev->gpio_module];
    u32 val;

    val = readl(base + GPIO_OE);
    val &= ~(1 << dev->gpio_pin);   /* Xoa bit = 0 -> OUTPUT */
    writel(val, base + GPIO_OE);
}

/**
 * gpio_reg_set_input - Cau hinh chan GPIO lam INPUT
 */
static void gpio_reg_set_input(struct dht22_dev *dev)
{
    void __iomem *base = dev->gpio_regs[dev->gpio_module];
    u32 val;

    val = readl(base + GPIO_OE);
    val |= (1 << dev->gpio_pin);    /* Set bit = 1 -> INPUT */
    writel(val, base + GPIO_OE);
}

/**
 * gpio_reg_set_high - Keo chan GPIO len HIGH
 */
static void gpio_reg_set_high(struct dht22_dev *dev)
{
    void __iomem *base = dev->gpio_regs[dev->gpio_module];

    writel(1 << dev->gpio_pin, base + GPIO_SETDATAOUT);
}

/**
 * gpio_reg_set_low - Keo chan GPIO xuong LOW
 */
static void gpio_reg_set_low(struct dht22_dev *dev)
{
    void __iomem *base = dev->gpio_regs[dev->gpio_module];

    writel(1 << dev->gpio_pin, base + GPIO_CLEARDATAOUT);
}

/**
 * gpio_reg_read_pin - Doc muc logic hien tai cua chan GPIO
 */
static int gpio_reg_read_pin(struct dht22_dev *dev)
{
    void __iomem *base = dev->gpio_regs[dev->gpio_module];
    u32 val;

    val = readl(base + GPIO_DATAIN);
    return (val >> dev->gpio_pin) & 0x1;
}

/* ===========================================================================
 * GIAO THUC GIAO TIEP DHT22
 * =========================================================================== */

/**
 * dht22_read_raw_data - Doc 40 bit du lieu tu cam bien DHT22
 */
static int dht22_read_raw_data(struct dht22_dev *dev)
{
    unsigned long flags;
    int data[DHT22_DATA_BYTES] = {0};
    int i;
    int cnt;
    int checksum;

    /*
     * ===== BUOC 1: GUI TIN HIEU START =====
     */
    local_irq_save(flags);

    gpio_reg_set_output(dev);

    /* Keo LOW 2ms -> DHT22 nhan biet master dang yeu cau (DHT22 can > 1ms) */
    gpio_reg_set_low(dev);
    mdelay(2);

    /* Keo HIGH ~30us -> bao hieu ket thuc start pulse */
    gpio_reg_set_high(dev);
    udelay(30);

    /* Chuyen GPIO sang INPUT de doc du lieu */
    gpio_reg_set_input(dev);

    /*
     * ===== BUOC 2: DOI DHT22 PHAN HOI =====
     */
    cnt = 0;
    while (gpio_reg_read_pin(dev) == 1) {
        udelay(1);
        if (++cnt > DHT22_TIMEOUT_US) {
            local_irq_restore(flags);
            pr_err("DHT22: Timeout - khong nhan phan hoi (doi LOW)\n");
            return -ETIMEDOUT;
        }
    }

    cnt = 0;
    while (gpio_reg_read_pin(dev) == 0) {
        udelay(1);
        if (++cnt > DHT22_TIMEOUT_US) {
            local_irq_restore(flags);
            pr_err("DHT22: Timeout - phan hoi LOW qua dai\n");
            return -ETIMEDOUT;
        }
    }

    cnt = 0;
    while (gpio_reg_read_pin(dev) == 1) {
        udelay(1);
        if (++cnt > DHT22_TIMEOUT_US) {
            local_irq_restore(flags);
            pr_err("DHT22: Timeout - phan hoi HIGH qua dai\n");
            return -ETIMEDOUT;
        }
    }

    /*
     * ===== BUOC 3: DOC 40 BIT DU LIEU =====
     */
    for (i = 0; i < DHT22_DATA_BITS; i++) {
        cnt = 0;
        while (gpio_reg_read_pin(dev) == 0) {
            udelay(1);
            if (++cnt > DHT22_TIMEOUT_US) {
                local_irq_restore(flags);
                pr_err("DHT22: Timeout doc bit %d (LOW)\n", i);
                return -ETIMEDOUT;
            }
        }

        cnt = 0;
        while (gpio_reg_read_pin(dev) == 1) {
            udelay(1);
            if (++cnt > DHT22_TIMEOUT_US) {
                local_irq_restore(flags);
                pr_err("DHT22: Timeout doc bit %d (HIGH)\n", i);
                return -ETIMEDOUT;
            }
        }

        data[i / 8] <<= 1;
        if (cnt > 28)
            data[i / 8] |= 1;
    }

    local_irq_restore(flags);

    /*
     * ===== BUOC 4: KIEM TRA CHECKSUM =====
     */
    checksum = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
    if (checksum != data[4]) {
        pr_err("DHT22: Loi checksum! Tinh=0x%02X, Nhan=0x%02X\n",
               checksum, data[4]);
        pr_err("DHT22: Raw data: [%d] [%d] [%d] [%d] [%d]\n",
               data[0], data[1], data[2], data[3], data[4]);
        return -EIO;
    }

    /* Tinh toan du lieu theo chuan DHT22 */
    {
        int h = (data[0] << 8) | data[1];
        int t = ((data[2] & 0x7F) << 8) | data[3];
        
        dev->humidity_int = h / 10;
        dev->humidity_dec = h % 10;
        
        dev->temp_sign = (data[2] & 0x80) ? 1 : 0;
        dev->temp_int  = t / 10;
        dev->temp_dec  = t % 10;
    }

    /* Da comment lai de Terminal sach se hon khi chay cat */
    // pr_info("DHT22: Doc thanh cong - Do am: %d.%d%%, Nhiet do: %s%d.%d�C\n",
    //         dev->humidity_int, dev->humidity_dec,
    //         dev->temp_sign ? "-" : "", dev->temp_int, dev->temp_dec);

    return 0;
}

/* ===========================================================================
 * FILE OPERATIONS - CAC HAM XU LY FILE DEVICE
 * =========================================================================== */

/**
 * dht22_open - Duoc goi khi user-space mo /dev/dht22
 */
static int dht22_open(struct inode *inode, struct file *file)
{
    struct dht22_dev *dev;

    dev = container_of(inode->i_cdev, struct dht22_dev, cdev);
    file->private_data = dev;

    mutex_lock(&dev->lock);
    if (dev->is_open) {
        mutex_unlock(&dev->lock);
        pr_warn("DHT22: Thiet bi dang duoc su dung!\n");
        return -EBUSY;
    }
    dev->is_open = true;
    dev->has_read = false;  
    mutex_unlock(&dev->lock);

    /* Da comment lai de Terminal sach se hon */
    // pr_info("DHT22: Device opened (GPIO%d_%d = pin %d)\n",
    //         dev->gpio_module, dev->gpio_pin, dev->gpio_num);
    return 0;
}

/**
 * dht22_release - Duoc goi khi user-space dong /dev/dht22
 */
static int dht22_release(struct inode *inode, struct file *file)
{
    struct dht22_dev *dev = file->private_data;

    mutex_lock(&dev->lock);
    dev->is_open = false;
    mutex_unlock(&dev->lock);

    /* Da comment lai de Terminal sach se hon */
    // pr_info("DHT22: Device closed\n");
    return 0;
}

/**
 * dht22_read - Doc du lieu nhiet do & do am tu DHT22 (CHI DOC 1 LAN ROI THOAT)
 */
static ssize_t dht22_read(struct file *file, char __user *buf,
                           size_t len, loff_t *off)
{
    struct dht22_dev *dev = file->private_data;
    char kbuf[128];
    int data_len;
    int ret;

    /* Kiem tra offset bao hieu EOF de dung lenh cat */
    if (*off > 0)
        return 0;

    /* CHO DU 5 GIAY GIUA 2 LAN DOC */
    if (dev->has_read) {
        unsigned long next_read_time = dev->last_read_jiffies +
                                       msecs_to_jiffies(DHT22_READ_INTERVAL_MS);

        if (time_before(jiffies, next_read_time)) {
            unsigned long wait_ms = jiffies_to_msecs(next_read_time - jiffies);
            // pr_info("DHT22: Cho %lu ms truoc khi doc tiep...\n", wait_ms);

            msleep_interruptible(wait_ms);

            if (signal_pending(current)) {
                // pr_info("DHT22: Doc bi huy boi user (Ctrl+C)\n");
                return -ERESTARTSYS;
            }
        }
    }

    mutex_lock(&dev->lock);

    ret = dht22_read_raw_data(dev);
    if (ret < 0) {
        mutex_unlock(&dev->lock);
        return ret;
    }

    dev->last_read_jiffies = jiffies;
    dev->has_read = true;

    /* CHI IN NHIET DO */
    data_len = snprintf(kbuf, sizeof(kbuf),
                        "%s%d.%d\n",
                        dev->temp_sign ? "-" : "", dev->temp_int, dev->temp_dec);

    mutex_unlock(&dev->lock);

    if (len < data_len)
        data_len = len;

    if (copy_to_user(buf, kbuf, data_len)) {
        pr_err("DHT22: copy_to_user that bai!\n");
        return -EFAULT;
    }

    *off += data_len;

    return data_len;
}

/**
 * dht22_write - Thay doi chan GPIO tu user-space
 */
static ssize_t dht22_write(struct file *file, const char __user *buf,
                            size_t len, loff_t *off)
{
    struct dht22_dev *dev = file->private_data;
    char kbuf[16];
    int new_gpio;
    int new_module;
    int ret;

    if (len >= sizeof(kbuf))
        return -EINVAL;

    if (copy_from_user(kbuf, buf, len)) {
        pr_err("DHT22: copy_from_user that bai!\n");
        return -EFAULT;
    }
    kbuf[len] = '\0';

    ret = kstrtoint(kbuf, 10, &new_gpio);
    if (ret < 0) {
        pr_err("DHT22: Gia tri GPIO khong hop le: '%s'\n", kbuf);
        return -EINVAL;
    }

    if (new_gpio < 0 || new_gpio > 127) {
        pr_err("DHT22: GPIO %d ngoai pham vi (0-127)\n", new_gpio);
        return -EINVAL;
    }

    new_module = gpio_get_module(new_gpio);
    if (!dev->gpio_mapped[new_module]) {
        pr_err("DHT22: GPIO module %d chua duoc ioremap!\n", new_module);
        return -ENODEV;
    }

    mutex_lock(&dev->lock);
    gpio_update_pin(dev, new_gpio);
    mutex_unlock(&dev->lock);

    pr_info("DHT22: Da chuyen sang GPIO%d_%d (pin %d)\n",
            dev->gpio_module, dev->gpio_pin, dev->gpio_num);

    return len;
}

/* ===========================================================================
 * BANG FILE OPERATIONS
 * =========================================================================== */

static const struct file_operations dht22_fops = {
    .owner   = THIS_MODULE,
    .open    = dht22_open,       /* open("/dev/dht22", ...)  */
    .release = dht22_release,    /* close(fd)                */
    .read    = dht22_read,       /* read(fd, buf, len)       */
    .write   = dht22_write,      /* write(fd, buf, len)      */
};

/* ===========================================================================
 * MODULE INIT - KHOI TAO DRIVER
 * =========================================================================== */

static int __init dht22_init(void)
{
    int ret;
    int i;

    pr_info("DHT22: ===== Khoi tao driver cho BeagleBone Black =====\n");

    if (gpio_num < 0 || gpio_num > 127) {
        pr_err("DHT22: GPIO %d ngoai pham vi (0-127)\n", gpio_num);
        return -EINVAL;
    }

    dht22_device = kzalloc(sizeof(struct dht22_dev), GFP_KERNEL);
    if (!dht22_device) {
        pr_err("DHT22: Khong the cap phat bo nho!\n");
        return -ENOMEM;
    }

    gpio_update_pin(dht22_device, gpio_num);
    dht22_device->is_open = false;
    mutex_init(&dht22_device->lock);

    ret = alloc_chrdev_region(&dht22_device->dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err("DHT22: Khong the cap phat device number!\n");
        goto err_free_mem;
    }
    
    cdev_init(&dht22_device->cdev, &dht22_fops);
    dht22_device->cdev.owner = THIS_MODULE;

    ret = cdev_add(&dht22_device->cdev, dht22_device->dev_num, 1);
    if (ret < 0) {
        pr_err("DHT22: Khong the dang ky cdev!\n");
        goto err_unreg_chrdev;
    }

    dht22_device->dev_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(dht22_device->dev_class)) {
        pr_err("DHT22: Khong the tao device class!\n");
        ret = PTR_ERR(dht22_device->dev_class);
        goto err_cdev_del;
    }

    dht22_device->device = device_create(dht22_device->dev_class, NULL,
                                         dht22_device->dev_num, NULL,
                                         DEVICE_NAME);
    if (IS_ERR(dht22_device->device)) {
        pr_err("DHT22: Khong the tao device!\n");
        ret = PTR_ERR(dht22_device->device);
        goto err_class_destroy;
    }

    for (i = 0; i < GPIO_NUM_MODULES; i++) {
        dht22_device->gpio_regs[i] = ioremap(gpio_module_base[i], GPIO_REG_SIZE);
        if (!dht22_device->gpio_regs[i]) {
            pr_warn("DHT22: Khong the ioremap GPIO%d (0x%lX)\n",
                    i, gpio_module_base[i]);
            dht22_device->gpio_mapped[i] = false;
        } else {
            dht22_device->gpio_mapped[i] = true;
        }
    }

    if (!dht22_device->gpio_mapped[dht22_device->gpio_module]) {
        pr_err("DHT22: Module GPIO%d can thiet chua duoc map!\n",
               dht22_device->gpio_module);
        ret = -ENOMEM;
        goto err_iounmap;
    }

    pr_info("DHT22: ===== Driver khoi tao thanh cong! =====\n");
    pr_info("DHT22: Platform: BeagleBone Black (AM335x)\n");
    pr_info("DHT22: GPIO%d_%d (pin %d)\n",
            dht22_device->gpio_module, dht22_device->gpio_pin,
            dht22_device->gpio_num);
    pr_info("DHT22: Doc du lieu:  cat /dev/dht22\n");
    pr_info("DHT22: Doi GPIO pin: echo \"<gpio_num>\" > /dev/dht22\n");

    return 0;

err_iounmap:
    for (i = 0; i < GPIO_NUM_MODULES; i++) {
        if (dht22_device->gpio_mapped[i])
            iounmap(dht22_device->gpio_regs[i]);
    }
    device_destroy(dht22_device->dev_class, dht22_device->dev_num);
err_class_destroy:
    class_destroy(dht22_device->dev_class);
err_cdev_del:
    cdev_del(&dht22_device->cdev);
err_unreg_chrdev:
    unregister_chrdev_region(dht22_device->dev_num, 1);
err_free_mem:
    kfree(dht22_device);
    return ret;
}

/* ===========================================================================
 * MODULE EXIT - GO BO DRIVER
 * =========================================================================== */

static void __exit dht22_exit(void)
{
    int i;

    pr_info("DHT22: ===== Bat dau go bo driver =====\n");

    for (i = 0; i < GPIO_NUM_MODULES; i++) {
        if (dht22_device->gpio_mapped[i]) {
            iounmap(dht22_device->gpio_regs[i]);
        }
    }

    device_destroy(dht22_device->dev_class, dht22_device->dev_num);
    class_destroy(dht22_device->dev_class);
    cdev_del(&dht22_device->cdev);
    unregister_chrdev_region(dht22_device->dev_num, 1);
    kfree(dht22_device);

    pr_info("DHT22: ===== Driver da go bo thanh cong! =====\n");
}

/* ===========================================================================
 * DANG KY MODULE
 * =========================================================================== */

module_init(dht22_init);
module_exit(dht22_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DHT22 Driver - BeagleBone Black");
MODULE_DESCRIPTION("DHT22 Sensor Driver - Direct GPIO Register Access for AM335x");
MODULE_VERSION("1.0");

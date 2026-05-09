#include <linux/module.h>   
#include <linux/kernel.h>  
#include <linux/fs.h>       
#include <linux/cdev.h>    
#include <linux/device.h>   
#include <linux/uaccess.h>  
#include <linux/io.h>       
#include <linux/delay.h>   
#include <linux/ioctl.h>    
#include <linux/jiffies.h>  

#define I2C2_BASE_ADDR    0x4819C000UL
#define I2C2_MEM_SIZE     0x1000UL      

#define CM_PER_BASE          0x44E00000
#define CM_PER_I2C2_CLKCTRL  0x44
#define MODULEMODE_ENABLE    0x02

#define CONTROL_MODULE_BASE 0x44E10000
#define CONF_UART1_RTSN     0x97C   
#define CONF_UART1_CTSN     0x978   

// 0x73: Slew rate slow (1), Rx Active (1), Pull-up (1), PU Enable (0), Mode 3
#define PIN_MODE_I2C        0x73

#define I2C_SYSC          0x10          
#define I2C_IRQSTATUS_RAW 0x24         
#define I2C_IRQSTATUS     0x28  
#define I2C_IRQENABLE_CLR 0x30   
#define I2C_SYSS          0x90   
#define I2C_CNT           0x98   
#define I2C_DATA          0x9C   
#define I2C_CON           0xA4                  
#define I2C_OA            0xA8 
#define I2C_SA            0xAC                  
#define I2C_PSC           0xB0                  
#define I2C_SCLL          0xB4                  
#define I2C_SCLH          0xB8                   

#define I2C_CON_EN        (1u << 15)        
#define I2C_CON_MST       (1u << 10)
#define I2C_CON_TRX       (1u <<  9)    
#define I2C_CON_STP       (1u <<  1)  
#define I2C_CON_STT       (1u <<  0)    

#define I2C_STAT_XRDY     (1u <<  4)  
#define I2C_STAT_ARDY     (1u <<  2)   
#define I2C_STAT_NACK     (1u <<  1)       
#define I2C_STAT_AL       (1u <<  0)
#define I2C_SYSS_RDONE    (1u <<  0)

#define LCD_I2C_ADDR      0x27       
#define LCD_RS            (1u << 0)  
#define LCD_RW            (1u << 1)  
#define LCD_EN            (1u << 2)  
#define LCD_BL            (1u << 3)  

#define LCD_CLEAR         0x01  
#define LCD_HOME          0x02  
#define LCD_ENTRY_MODE    0x06  
#define LCD_DISP_OFF      0x08  
#define LCD_DISP_ON       0x0C  
#define LCD_FUNC_SET      0x28  
#define LCD_LINE1         0x80  
#define LCD_LINE2         0xC0  
#define LCD_COLS          16    

#define LCD_IOC_MAGIC     'L' 
#define LCD_IOCTL_CLEAR   _IO (LCD_IOC_MAGIC, 0)  
#define LCD_IOCTL_HOME    _IO (LCD_IOC_MAGIC, 1)  
#define LCD_IOCTL_SETPOS  _IOW(LCD_IOC_MAGIC, 2, unsigned int)
#define LCD_IOCTL_INIT    _IO (LCD_IOC_MAGIC, 3)

#define DEVICE_NAME       "lcd_i2c"  
#define CLASS_NAME        "lytn_lcd_class"      
#define I2C_TIMEOUT_MS    100        

static void __iomem  *i2c2_base;  
static dev_t          dev_num;    
static struct cdev    lcd_cdev;   
static struct class  *lcd_class;  
static struct device *lcd_dev;    

static u8 cursor_line;  
static u8 cursor_col;   

static int i2c_poll_bit(u32 reg_off, u32 mask) 
{
    unsigned long deadline = jiffies + msecs_to_jiffies(I2C_TIMEOUT_MS);
    while (!(ioread32(i2c2_base + reg_off) & mask)) {
        if (time_after(jiffies, deadline))
            return -ETIMEDOUT;  
        cpu_relax();            
    }
    return 0;  
}

static int i2c_write_byte(u8 data)
{
    int ret;
    iowrite32(0xFFFF, i2c2_base + I2C_IRQSTATUS);
    iowrite32(1, i2c2_base + I2C_CNT);
    iowrite32(I2C_CON_EN | I2C_CON_MST | I2C_CON_TRX |
              I2C_CON_STT | I2C_CON_STP, i2c2_base + I2C_CON);

    ret = i2c_poll_bit(I2C_IRQSTATUS_RAW, I2C_STAT_XRDY);
    if (ret) return ret;

    iowrite32(data, i2c2_base + I2C_DATA);
    iowrite32(I2C_STAT_XRDY, i2c2_base + I2C_IRQSTATUS);

    ret = i2c_poll_bit(I2C_IRQSTATUS_RAW, I2C_STAT_ARDY);
    if (ret) return ret;

    iowrite32(I2C_STAT_ARDY, i2c2_base + I2C_IRQSTATUS);
    return 0;  
}

static int i2c_hw_init(void)
{
    /* Mồi module dậy trước khi Reset */
    iowrite32(I2C_CON_EN, i2c2_base + I2C_CON);
    udelay(100);

    /* Soft-reset */
    iowrite32(0x0002, i2c2_base + I2C_SYSC);
    mdelay(10);  /* Chờ lâu hơn để phần cứng kịp phản ứng */

    /* Nếu Timeout -> Cảnh báo rồi BỎ QUA, bắt CPU chạy tiếp */
    if (i2c_poll_bit(I2C_SYSS, I2C_SYSS_RDONE)) {
        printk(KERN_WARNING "lcd_i2c: I2C2 reset timeout nhung toi se ep chay tiep!\n");
    }

    /* Bắt buộc tắt module để cấu hình Clock */
    iowrite32(0, i2c2_base + I2C_CON);

    iowrite32(3,  i2c2_base + I2C_PSC);
    iowrite32(53, i2c2_base + I2C_SCLL);
    iowrite32(55, i2c2_base + I2C_SCLH);

    iowrite32(0, i2c2_base + I2C_OA);
    iowrite32(LCD_I2C_ADDR, i2c2_base + I2C_SA);
    iowrite32(0xFFFF, i2c2_base + I2C_IRQENABLE_CLR);

    iowrite32(I2C_CON_EN, i2c2_base + I2C_CON);
    udelay(100);  

    return 0;
}

static void lcd_pulse_enable(u8 val) 
{
    i2c_write_byte(val | LCD_EN);   
    udelay(1);                      
    i2c_write_byte(val & ~LCD_EN);  
    udelay(50);                     
}

static void lcd_send_cmd(u8 cmd) 
{
    u8 hi = (cmd & 0xF0) | LCD_BL; 
    u8 lo = ((cmd << 4) & 0xF0) | LCD_BL; 
    i2c_write_byte(hi); 
    lcd_pulse_enable(hi);
    i2c_write_byte(lo); 
    lcd_pulse_enable(lo);
    if (cmd == LCD_CLEAR || cmd == LCD_HOME) 
        mdelay(2);
    else
        udelay(50);   
}

static void lcd_send_char(u8 ch)
{
    u8 hi = (ch & 0xF0) | LCD_RS | LCD_BL; 
    u8 lo = ((ch << 4) & 0xF0) | LCD_RS | LCD_BL; 

    i2c_write_byte(hi); 
    lcd_pulse_enable(hi); 
    i2c_write_byte(lo); 
    lcd_pulse_enable(lo); 

    udelay(50);  
}

static void lcd_set_cursor(u8 line, u8 col) 
{
    u8 addr = (line == 0) ? (LCD_LINE1 + col) : (LCD_LINE2 + col);
    lcd_send_cmd(addr);
    cursor_line = line;
    cursor_col  = col;
}

static void lcd_init(void)
{
    mdelay(50);  
    i2c_write_byte(0x30 | LCD_BL);
    lcd_pulse_enable(0x30 | LCD_BL);
    mdelay(5);

    i2c_write_byte(0x30 | LCD_BL); 
    lcd_pulse_enable(0x30 | LCD_BL);
    udelay(150);

    i2c_write_byte(0x30 | LCD_BL); 
    lcd_pulse_enable(0x30 | LCD_BL);
    udelay(150);

    i2c_write_byte(0x20 | LCD_BL); 
    lcd_pulse_enable(0x20 | LCD_BL);
    udelay(150); 

    lcd_send_cmd(LCD_FUNC_SET);
    lcd_send_cmd(LCD_DISP_OFF);
    lcd_send_cmd(LCD_CLEAR);
    lcd_send_cmd(LCD_ENTRY_MODE);
    lcd_send_cmd(LCD_DISP_ON);

    cursor_line = 0;
    cursor_col  = 0;
}

static int lcd_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "lcd_i2c: thiết bị được mở\n");
    return 0;  
}

static int lcd_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "lcd_i2c: thiết bị được đóng\n");
    return 0;
}

static ssize_t lcd_write(struct file *file, const char __user *buf,
                         size_t count, loff_t *ppos)
{
    char kbuf[256];  
    size_t i;

    if (count > sizeof(kbuf) - 1) count = sizeof(kbuf) - 1;
    if (copy_from_user(kbuf, buf, count)) return -EFAULT;  

    kbuf[count] = '\0';  

    if (i2c_write_byte(LCD_BL) < 0) {
        printk(KERN_ERR "lcd_i2c: Mat ket noi I2C voi man hinh!\n");
        return -EIO; // Lập tức trả lỗi Input/Output về cho app C ở User-space
    }

    for (i = 0; i < count; i++) {
        if (kbuf[i] == '\n') {
            if (cursor_line == 0) {
                lcd_set_cursor(1, 0);  
            } else {
                lcd_send_cmd(LCD_CLEAR);
                lcd_set_cursor(0, 0);
            }
        } else {
            if (cursor_col >= LCD_COLS) {
                if (cursor_line == 0) {
                    lcd_set_cursor(1, 0);  
                } else {
                    lcd_send_cmd(LCD_CLEAR);
                    lcd_set_cursor(0, 0);  
                }
            }
            lcd_send_char(kbuf[i]);
            cursor_col++;
        }
    }
    return count;  
}

static ssize_t lcd_read(struct file *file, char __user *buf,
                        size_t count, loff_t *ppos)
{
    return 0;
}

static long lcd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    unsigned int pos;

    switch (cmd) {
    case LCD_IOCTL_CLEAR:
        lcd_send_cmd(LCD_CLEAR);
        cursor_line = 0;
        cursor_col  = 0;
        break;

    case LCD_IOCTL_HOME:
        lcd_send_cmd(LCD_HOME);
        cursor_line = 0;
        cursor_col  = 0;
        break;

    case LCD_IOCTL_SETPOS:
        if (copy_from_user(&pos, (unsigned int __user *)arg, sizeof(pos)))
            return -EFAULT; 
        lcd_set_cursor((u8)(pos >> 8) & 0x01, (u8)(pos & 0xFF) % LCD_COLS);
        break;
    case LCD_IOCTL_INIT:
    	i2c_hw_init();
        lcd_init(); 
        cursor_line = 0;
        cursor_col  = 0;
        break;

    default:
        return -EINVAL;  
    }
    return 0;
}

static const struct file_operations lcd_fops = {
    .owner          = THIS_MODULE,  
    .open           = lcd_open,
    .release        = lcd_release,
    .read           = lcd_read,
    .write          = lcd_write,
    .unlocked_ioctl = lcd_ioctl,
};

static int __init i2clcd_init(void)
{
    int ret;
    void __iomem *cm_per;
    void __iomem *ctrl_mod;

    cm_per = ioremap(CM_PER_BASE, 0x1000);
    if (!cm_per) {
        printk(KERN_ERR "lcd_i2c: Khong the remap CM_PER\n");
        return -ENOMEM;
    }

    iowrite32(MODULEMODE_ENABLE, cm_per + CM_PER_I2C2_CLKCTRL);
    udelay(100); 

    if ((ioread32(cm_per + CM_PER_I2C2_CLKCTRL) & 0x30000) != 0) {
        printk(KERN_ERR "lcd_i2c: I2C2 clock chua san sang!\n");
        iounmap(cm_per);
        return -EIO;
    }
    iounmap(cm_per);
    
    ctrl_mod = ioremap(CONTROL_MODULE_BASE, 0x2000);
    if (ctrl_mod) {
        iowrite32(PIN_MODE_I2C, ctrl_mod + CONF_UART1_RTSN); 
        iowrite32(PIN_MODE_I2C, ctrl_mod + CONF_UART1_CTSN); 
        iounmap(ctrl_mod);
        printk(KERN_INFO "Lytn_04: Pinmux I2C Mode 3 (0x73) thành công!\n");
    }

    i2c2_base = ioremap(I2C2_BASE_ADDR, I2C2_MEM_SIZE); 
    if (!i2c2_base) {
        printk(KERN_ERR "lcd_i2c: ioremap thất bại\n");
        return -ENOMEM;
    }

    ret = i2c_hw_init();
    if (ret) goto err_iounmap;

    lcd_init();

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret) goto err_iounmap;

    cdev_init(&lcd_cdev, &lcd_fops);
    lcd_cdev.owner = THIS_MODULE;

    ret = cdev_add(&lcd_cdev, dev_num, 1);
    if (ret) goto err_unreg_chrdev;

    lcd_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(lcd_class)) {
        ret = PTR_ERR(lcd_class);  
        goto err_cdev_del;
    }

    lcd_dev = device_create(lcd_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(lcd_dev)) {
        ret = PTR_ERR(lcd_dev);
        goto err_class_destroy;
    }

    printk(KERN_INFO "lcd_i2c: driver nạp thành công - /dev/%s sẵn sàng\n", DEVICE_NAME);
    return 0;

err_class_destroy:
    class_destroy(lcd_class);     
err_cdev_del:
    cdev_del(&lcd_cdev);          
err_unreg_chrdev:
    unregister_chrdev_region(dev_num, 1);  
err_iounmap:
    iounmap(i2c2_base);           
    return ret;
}

static void __exit i2clcd_exit(void) 
{
    lcd_send_cmd(LCD_CLEAR);
    i2c_write_byte(0x00);  

    device_destroy(lcd_class, dev_num);       
    class_destroy(lcd_class);                 
    cdev_del(&lcd_cdev);                      
    unregister_chrdev_region(dev_num, 1);     
    iounmap(i2c2_base);                       

    printk(KERN_INFO "lcd_i2c: driver đã được gỡ tải\n");
}

module_init(i2clcd_init); 
module_exit(i2clcd_exit); 

MODULE_LICENSE("GPL");                    
MODULE_AUTHOR("Lytn");
MODULE_DESCRIPTION("LCD I2C2 16x2 driver for BeagleBone Black");
MODULE_VERSION("1.0");

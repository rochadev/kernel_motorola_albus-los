#include "nouveau_drm.h"
#include "nouveau_compat.h"

#include <subdev/bios.h>
#include <subdev/gpio.h>
#include <subdev/i2c.h>

void *nouveau_newpriv(struct drm_device *);

u8
_nv_rd08(struct drm_device *dev, u32 reg)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	return nv_ro08(drm->device, reg);
}

void
_nv_wr08(struct drm_device *dev, u32 reg, u8 val)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	nv_wo08(drm->device, reg, val);
}

u32
_nv_rd32(struct drm_device *dev, u32 reg)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	return nv_ro32(drm->device, reg);
}

void
_nv_wr32(struct drm_device *dev, u32 reg, u32 val)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	nv_wo32(drm->device, reg, val);
}

u32
_nv_mask(struct drm_device *dev, u32 reg, u32 mask, u32 val)
{
	u32 tmp = _nv_rd32(dev, reg);
	_nv_wr32(dev, reg, (tmp & ~mask) | val);
	return tmp;
}

bool
_nv_bios(struct drm_device *dev, u8 **data, u32 *size)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_bios *bios = nouveau_bios(drm->device);
	*data = bios->data;
	*size = bios->size;
	return true;
}

void
nouveau_gpio_reset(struct drm_device *dev)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_gpio *gpio = nouveau_gpio(drm->device);
	gpio->reset(gpio);
}

int
nouveau_gpio_find(struct drm_device *dev, int idx, u8 tag, u8 line,
		  struct dcb_gpio_func *func)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_gpio *gpio = nouveau_gpio(drm->device);

	return gpio->find(gpio, idx, tag, line, func);
}

bool
nouveau_gpio_func_valid(struct drm_device *dev, u8 tag)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_gpio *gpio = nouveau_gpio(drm->device);
	struct dcb_gpio_func func;

	return gpio->find(gpio, 0, tag, 0xff, &func) == 0;
}

int
nouveau_gpio_func_set(struct drm_device *dev, u8 tag, int state)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_gpio *gpio = nouveau_gpio(drm->device);
	if (gpio && gpio->get)
		return gpio->set(gpio, 0, tag, 0xff, state);
	return -ENODEV;
}

int
nouveau_gpio_func_get(struct drm_device *dev, u8 tag)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_gpio *gpio = nouveau_gpio(drm->device);
	if (gpio && gpio->get)
		return gpio->get(gpio, 0, tag, 0xff);
	return -ENODEV;
}

int
nouveau_gpio_irq(struct drm_device *dev, int idx, u8 tag, u8 line, bool on)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_gpio *gpio = nouveau_gpio(drm->device);
	if (gpio && gpio->irq)
		return gpio->irq(gpio, idx, tag, line, on);
	return -ENODEV;
}

int
nouveau_gpio_isr_add(struct drm_device *dev, int idx, u8 tag, u8 line,
		     void (*exec)(void *, int state), void *data)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_gpio *gpio = nouveau_gpio(drm->device);
	if (gpio && gpio->isr_add)
		return gpio->isr_add(gpio, idx, tag, line, exec, data);
	return -ENODEV;
}

void
nouveau_gpio_isr_del(struct drm_device *dev, int idx, u8 tag, u8 line,
		     void (*exec)(void *, int state), void *data)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_gpio *gpio = nouveau_gpio(drm->device);
	if (gpio && gpio->isr_del)
		gpio->isr_del(gpio, idx, tag, line, exec, data);
}

struct nouveau_i2c_port *
nouveau_i2c_find(struct drm_device *dev, u8 index)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_i2c *i2c = nouveau_i2c(drm->device);

	return i2c->find(i2c, index);
}

bool
nouveau_probe_i2c_addr(struct nouveau_i2c_port *port, int addr)
{
	return nv_probe_i2c(port, addr);
}

struct i2c_adapter *
nouveau_i2c_adapter(struct nouveau_i2c_port *port)
{
	return &port->adapter;
}


int
nouveau_i2c_identify(struct drm_device *dev, const char *what,
		     struct i2c_board_info *info,
		     bool (*match)(struct nouveau_i2c_port *,
			           struct i2c_board_info *),
		     int index)
{
	struct nouveau_drm *drm = nouveau_newpriv(dev);
	struct nouveau_i2c *i2c = nouveau_i2c(drm->device);

	return i2c->identify(i2c, index, what, info, match);
}

int
auxch_rd(struct drm_device *dev, struct nouveau_i2c_port *port,
	 u32 addr, u8 *data, u8 size)
{
	return nv_rdaux(port, addr, data, size);
}

int
auxch_wr(struct drm_device *dev, struct nouveau_i2c_port *port,
	 u32 addr, u8 *data, u8 size)
{
	return nv_wraux(port, addr, data, size);
}

#
# Makefile for the linux kernel.
#

# Common support
obj-y := common.o dma.o clock.o mux.o gpio.o mcbsp.o usb.o
obj-m :=
obj-n :=
obj-  :=

# OCPI interconnect support for 1710, 1610 and 5912
obj-$(CONFIG_ARCH_OMAP16XX) += ocpi.o

# Power Management
obj-$(CONFIG_PM) += pm.o sleep.o

obj-$(CONFIG_CPU_FREQ) += cpu-omap.o

#
# Makefile for ALSA
# Copyright (c) 2001 by Jaroslav Kysela <perex@suse.cz>
#

snd-powermac-objs := powermac.o pmac.o awacs.o burgundy.o daca.o tumbler.o keywest.o beep.o
snd-gcn-objs := gcn-ai.o

# Toplevel Module Dependency
obj-$(CONFIG_SND_POWERMAC) += snd-powermac.o
obj-$(CONFIG_SND_GAMECUBE) += snd-gcn.o
obj-$(CONFIG_SND_GAMECUBE_MIC) += gcn-mic.o

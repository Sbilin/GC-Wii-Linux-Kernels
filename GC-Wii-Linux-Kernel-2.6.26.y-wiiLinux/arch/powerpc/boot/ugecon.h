/*
 * arch/powerpc/boot/ugecon.h
 *
 * USB Gecko early bootwrapper console.
 * Copyright (C) 2008 The GameCube Linux Team
 * Copyright (C) 2008 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __UGECON_H
#define __UGECON_H

extern int ug_grab_io_base(void);
extern int ug_is_adapter_present(void);

extern void ug_putc(char ch);

#endif /* __UGECON_H */


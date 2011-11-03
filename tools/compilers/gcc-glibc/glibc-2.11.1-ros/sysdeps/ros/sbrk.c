/* Copyright (C) 1991,1995-1997,2000,2002,2009 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <bits/libc-lock.h>
#include <ros/syscall.h>
#include <ros/memlayout.h>
#include <ros/procinfo.h>
#include <sys/mman.h>

__libc_lock_define(static,brk_lock);
static uintptr_t curbrk = 0;

static uintptr_t
__internal_getbrk (void)
{
  if(curbrk == 0)
    curbrk = (uintptr_t)__procinfo.heap_bottom;
  return curbrk;
}

static int
__internal_setbrk (uintptr_t addr)
{
  uintptr_t real_new_brk = (addr + PGSIZE - 1)/PGSIZE*PGSIZE;
  uintptr_t real_brk = (__internal_getbrk() + PGSIZE - 1)/PGSIZE*PGSIZE;

  if(real_new_brk > real_brk)
  {
    if(real_new_brk > BRK_END)
      return -1;
	// calling mmap directly to avoid referencing errno before it is initialized.
    if ((void*)__ros_syscall(SYS_mmap, (void*)real_brk, real_new_brk-real_brk,
             PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_FIXED | MAP_ANONYMOUS, -1, 0, NULL) != (void*)real_brk)
      return -1;
  }
  else if(real_new_brk < real_brk)
  {
    if(real_new_brk < (uintptr_t)__procinfo.heap_bottom)
      return -1;

    if (munmap((void*)real_new_brk, real_brk - real_new_brk))
      return -1;
  }

  curbrk = addr;
  return 0;
}

/* Set the end of the process's data space to ADDR.
   Return 0 if successful, -1 if not.   */
int
__brk (void* addr)
{
  if(addr == 0)
    return 0;

  __libc_lock_lock(brk_lock);
  int ret = __internal_setbrk((uintptr_t)addr);
  __libc_lock_unlock(brk_lock);

  return ret;
}
weak_alias (__brk, brk)

/* Extend the process's data space by INCREMENT.
   If INCREMENT is negative, shrink data space by - INCREMENT.
   Return start of new space allocated, or -1 for errors.  */
void *
__sbrk (intptr_t increment)
{
  __libc_lock_lock(brk_lock);

  uintptr_t oldbrk = __internal_getbrk();
  if ((increment > 0
       ? (oldbrk + (uintptr_t) increment < oldbrk)
       : (oldbrk < (uintptr_t) -increment))
      || __internal_setbrk (oldbrk + increment) < 0)
    oldbrk = -1;

  __libc_lock_unlock(brk_lock);

  return (void*)oldbrk;
}
libc_hidden_def (__sbrk)
weak_alias (__sbrk, sbrk)

/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

   Inspired by and based on code from sd2iec, written by Ingo Korb et al.
   See sdcard.c|h, config.h.

   FAT file system access based on code by ChaN, Jim Brain, Ingo Korb,
   see ff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   filetypes.c: directory scanning and file type detection
*/

#include <string.h>
#include "config.h"
#include "uart.h"
#include "filetypes.h"
#include "ff.h"
#include "smc.h"
#include "fileops.h"
#include "crc.h"
#include "memory.h"
#include "led.h"
#include "sort.h"

uint16_t scan_flat(const char* path) {
  DIR dir;
  FRESULT res;
  FILINFO fno;
  fno.lfname = NULL;
  res = f_opendir(&dir, (TCHAR*)path);
  uint16_t numentries = 0;
  if (res == FR_OK) {
    for (;;) {
      res = f_readdir(&dir, &fno);
      if(res != FR_OK || fno.fname[0] == 0)break;
      numentries++;
    }
  }
  return numentries;
}

uint32_t scan_dir(char* path, FILINFO* fno_param, char mkdb, uint32_t this_dir_tgt) {
  DIR dir;
  FILINFO fno;
  FRESULT res;
  uint8_t len;
  TCHAR* fn;
  static unsigned char depth = 0;
  static uint32_t crc, fncrc;
  static uint32_t db_tgt;
  static uint32_t next_subdir_tgt;
  static uint32_t parent_tgt;
  static uint32_t dir_end = 0;
/*  static uint8_t was_empty = 0;*/
  static uint16_t num_files_total = 0;
  static uint16_t num_dirs_total = 0;
  uint32_t dir_tgt;
  uint32_t switched_dir_tgt = 0;
  uint16_t numentries;
  uint32_t dirsize;
  uint8_t pass = 0;
  char buf[7];
  char *size_units[3] = {" ", "k", "M"};
  uint32_t entry_fsize;
  uint8_t entry_unit_idx;
  uint16_t entrycnt;

  dir_tgt = this_dir_tgt;
  if(depth==0) {
    crc = 0;
    db_tgt = SRAM_DB_ADDR+0x10;
    dir_tgt = SRAM_DIR_ADDR;
    next_subdir_tgt = SRAM_DIR_ADDR;
    this_dir_tgt = SRAM_DIR_ADDR;
    parent_tgt = 0;
    printf("root dir @%lx\n", dir_tgt);
  }

  fno.lfsize = 255;
  fno.lfname = (TCHAR*)file_lfn;
  numentries=0;
  for(pass = 0; pass < (mkdb ? 2 : 1); pass++) {
    if(pass) {
      dirsize = 4*(numentries);
      if(((next_subdir_tgt + dirsize + 8) & 0xff0000) > (next_subdir_tgt & 0xff0000)) {
        printf("switchdir! old=%lX ", next_subdir_tgt + dirsize + 4);
        next_subdir_tgt &= 0xffff0000;
        next_subdir_tgt += 0x00010004;
        printf("new=%lx\n", next_subdir_tgt);
        dir_tgt &= 0xffff0000;
        dir_tgt += 0x00010004;
      }
      switched_dir_tgt = dir_tgt;
      next_subdir_tgt += dirsize + 4;
      if(parent_tgt) next_subdir_tgt += 4;
      if(next_subdir_tgt > dir_end) {
        dir_end = next_subdir_tgt;
      }
      DBG_FS printf("path=%s depth=%d ptr=%lx entries=%d parent=%lx next subdir @%lx\n", path, depth, db_tgt, numentries, parent_tgt, next_subdir_tgt);
      if(mkdb) {
        num_dirs_total++;
//        printf("d=%d Saving %lx to Address %lx  [end]\n", depth, 0L, next_subdir_tgt - 4);
        sram_writelong(0L, next_subdir_tgt - 4);
      }
    }
    if(fno_param) {
      res = dir_open_by_filinfo(&dir, fno_param);
    } else {
      res = f_opendir(&dir, path);
    }
    if (res == FR_OK) {
      if(pass && parent_tgt && mkdb) {
        /* write backlink to parent dir
           switch to next bank if record does not fit in current bank */
        if((db_tgt&0xffff) > ((0x10000-(sizeof(next_subdir_tgt)+sizeof(len)+4))&0xffff)) {
          printf("switch! old=%lx ", db_tgt);
          db_tgt &= 0xffff0000;
          db_tgt += 0x00010000;
          printf("new=%lx\n", db_tgt);
        }
//        printf("writing link to parent, %lx to address %lx [../]\n", parent_tgt-SRAM_MENU_ADDR, db_tgt);
        sram_writelong((parent_tgt-SRAM_MENU_ADDR), db_tgt);
        sram_writebyte(0, db_tgt+sizeof(next_subdir_tgt));
        sram_writeblock("../\0", db_tgt+sizeof(next_subdir_tgt)+sizeof(len), 4);
        sram_writelong((db_tgt-SRAM_MENU_ADDR)|((uint32_t)0x81<<24), dir_tgt);
        db_tgt += sizeof(next_subdir_tgt)+sizeof(len)+4;
        dir_tgt += 4;
      }
      len = strlen((char*)path);
      /* scan at most DIR_FILE_MAX entries per directory */
      for(entrycnt=0; entrycnt < DIR_FILE_MAX; entrycnt++) {
//        toggle_read_led();
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
          if(pass) {
/*            if(!numentries) was_empty=1;*/
          }
          break;
        }
        fn = *fno.lfname ? fno.lfname : fno.fname;
        if ((*fn == '.') || (fno.fattrib & (AM_HID | AM_SYS)) || !(strncasecmp(fn, SYS_DIR_NAME, strlen(SYS_DIR_NAME)+1))) continue;
        if (fno.fattrib & AM_DIR) {
          depth++;
          if(depth < FS_MAX_DEPTH) {
            numentries++;
            if(pass && mkdb) {
              path[len]='/';
              strncpy(path+len+1, (char*)fn, sizeof(fs_path)-len);
              uint16_t pathlen = 0;
              uint32_t old_db_tgt = 0;
              if(mkdb) {
                pathlen = strlen(path);
                DBG_FS printf("d=%d Saving %lx to Address %lx  [dir]\n", depth, db_tgt, dir_tgt);
                /* save element:
                   - path name
                   - pointer to sub dir structure */
                if((db_tgt&0xffff) > ((0x10000-(sizeof(next_subdir_tgt) + sizeof(len) + pathlen + 2))&0xffff)) {
                  printf("switch! old=%lx ", db_tgt);
                  db_tgt &= 0xffff0000;
                  db_tgt += 0x00010000;
                  printf("new=%lx\n", db_tgt);
                }
                /* write element pointer to current dir structure */
                sram_writelong((db_tgt-SRAM_MENU_ADDR)|((uint32_t)0x80<<24), dir_tgt);
                /* save element:
                   - path name
                   - pointer to sub dir structure
                   moved below */
                old_db_tgt = db_tgt;
                db_tgt += sizeof(next_subdir_tgt) + sizeof(len) + pathlen + 2;
              }
              parent_tgt = this_dir_tgt;
              /* scan subdir before writing current dir element to account for bank switches */
              uint32_t corrected_subdir_tgt = scan_dir(path, &fno, mkdb, next_subdir_tgt);
              if(mkdb) {
                DBG_FS printf("    Saving dir descriptor to %lx tgt=%lx, path=%s\n", old_db_tgt, corrected_subdir_tgt, path);
                sram_writelong((corrected_subdir_tgt-SRAM_MENU_ADDR), old_db_tgt);
                sram_writebyte(len+1, old_db_tgt+sizeof(next_subdir_tgt));
                sram_writeblock(path, old_db_tgt+sizeof(next_subdir_tgt)+sizeof(len), pathlen);
                sram_writeblock("/\0", old_db_tgt + sizeof(next_subdir_tgt) + sizeof(len) + pathlen, 2);
              }
              dir_tgt += 4;
/*              was_empty = 0;*/
            } else if(!mkdb) {
              path[len]='/';
              strncpy(path+len+1, (char*)fn, sizeof(fs_path)-len);
              scan_dir(path, &fno, mkdb, next_subdir_tgt);
            }
          }
          depth--;
          path[len]=0;
        } else {
          SNES_FTYPE type = determine_filetype((char*)fn);
          if(type != TYPE_UNKNOWN) {
            numentries++;
            if(pass) {
              if(mkdb) {
                num_files_total++;
/*                snes_romprops_t romprops; */
                path[len]='/';
                strncpy(path+len+1, (char*)fn, sizeof(fs_path)-len);
                uint16_t pathlen = strlen(path);
                switch(type) {
                  case TYPE_IPS:
                  case TYPE_SMC:
                  case TYPE_SPC:
                    /* write element pointer to current dir structure */
                    DBG_FS printf("d=%d Saving %lX to Address %lX  [file %s]\n", depth, db_tgt, dir_tgt, path);
                    if((db_tgt&0xffff) > ((0x10000-(sizeof(len) + pathlen + sizeof(buf)-1 + 1))&0xffff)) {
                      printf("switch! old=%lx ", db_tgt);
                      db_tgt &= 0xffff0000;
                      db_tgt += 0x00010000;
                      printf("new=%lx\n", db_tgt);
                    }
                    sram_writelong((db_tgt-SRAM_MENU_ADDR) | ((uint32_t)type << 24), dir_tgt);
                    dir_tgt += 4;
                    /* save element:
                        - index of last slash character
                        - file name
                        - file size */
/*                  sram_writeblock((uint8_t*)&romprops, db_tgt, sizeof(romprops)); */
                    entry_fsize = fno.fsize;
                    entry_unit_idx = 0;
                    while(entry_fsize > 9999) {
                      entry_fsize >>= 10;
                      entry_unit_idx++;
                    }
                    snprintf(buf, sizeof(buf), "% 5ld", entry_fsize);
                    strncat(buf, size_units[entry_unit_idx], 1);
                    sram_writeblock(buf, db_tgt, sizeof(buf)-1);
                    sram_writebyte(len+1, db_tgt + sizeof(buf)-1);
                    sram_writeblock(path, db_tgt + sizeof(len) + sizeof(buf)-1, pathlen + 1);
//                    sram_writelong(fno.fsize, db_tgt + sizeof(len) + pathlen + 1);
                    db_tgt += sizeof(len) + pathlen + sizeof(buf)-1 + 1;
                    break;
                  case TYPE_UNKNOWN:
                  default:
                   break;
                }
                path[len] = 0;
/*              printf("%s ", path);
                _delay_ms(30); */
              }
            } else {
              TCHAR* fn2 = fn;
              fncrc = 0;
              while(*fn2 != 0) {
                fncrc += crc_xmodem_update(fncrc, *((unsigned char*)fn2++));
              }
              crc += fncrc;
            }
          }
        }
      }
    } else uart_putc(0x30+res);
  }
  DBG_FS printf("db_tgt=%lx dir_end=%lx\n", db_tgt, dir_end);
  sram_writelong(db_tgt, SRAM_DB_ADDR+4);
  sram_writelong(dir_end, SRAM_DB_ADDR+8);
  sram_writeshort(num_files_total, SRAM_DB_ADDR+12);
  sram_writeshort(num_dirs_total, SRAM_DB_ADDR+14);
  if(depth==0) return crc;
  else return switched_dir_tgt;
}


SNES_FTYPE determine_filetype(char* filename) {
  char* ext = strrchr(filename, '.');
  if(ext == NULL)
    return TYPE_UNKNOWN;
  if(  (!strcasecmp(ext+1, "SMC"))
     ||(!strcasecmp(ext+1, "SFC"))
     ||(!strcasecmp(ext+1, "FIG"))
     ||(!strcasecmp(ext+1, "BS"))
     ||(!strcasecmp(ext+1, "ST"))
    ) {
    return TYPE_SMC;
  }
/*  if(  (!strcasecmp(ext+1, "IPS"))
     ||(!strcasecmp(ext+1, "UPS"))
    ) {
    return TYPE_IPS;
  }*/
  if(!strcasecmp(ext+1, "SPC")) {
    return TYPE_SPC;
  }
  return TYPE_UNKNOWN;
}

FRESULT get_db_id(uint32_t* id) {
  file_open((uint8_t*)"/sd2snes/sd2snes.db", FA_READ);
  if(file_res == FR_OK) {
    file_readblock(id, 0, 4);
/* XXX */// *id=0xdead;
    file_close();
  } else {
    *id=0xdeadbeef;
  }
  return file_res;
}

int get_num_dirent(uint32_t addr) {
  int result = 0;
  while(sram_readlong(addr+result*4)) {
    result++;
  }
  return result;
}

void sort_all_dir(uint32_t endaddr) {
  uint32_t entries = 0;
  uint32_t current_base = SRAM_DIR_ADDR;
  while(current_base<(endaddr)) {
    while(sram_readlong(current_base+entries*4)) {
      entries++;
    }
    printf("sorting dir @%lx, entries: %ld\n", current_base, entries);
    sort_dir(current_base, entries);
    current_base += 4*entries + 4;
    entries = 0;
  }
}

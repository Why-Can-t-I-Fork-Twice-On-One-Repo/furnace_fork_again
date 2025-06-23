/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2025 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

// thanks asiekierka!
// I have ported your code to this ROM export framework.

#include "sndh.h"
#include "../engine.h"
#include "../ta-log.h"
extern "C" {
  #include "../../../extern/pack-ice/ice.h"
}
#include <fmt/printf.h>
#include <array>
#include <vector>

class BackRef {
  public:
    int pos;
    inline void add(short val) {
      data[pos]=val;
      pos=(pos+1)&0x7f;
    };
    inline short operator[](int addr) {
      return data[addr];
    }
    inline short rel(int addr) {
      return data[(addr+pos)&0x7f];
    }
    short find(short val) {
      short i;
      for (i=0; i<128; i++) {
        if (data[i]==val) break;
      }
      if (i==128) i=-1;
      return i;
    }
    BackRef(): pos(0) {
      memset(data,0,sizeof(data));
    };
  private:
    short data[128];
};

static void writeTextTag(SafeWriter *w, const char* tag, String s) {
  if (s.size()==0) return;
  w->writeString(tag+s,false);
}

static void writeWait(std::vector<unsigned char>& data, BackRef& backref, int& newWait) {
  while (newWait>0) {
    int thisWait=MIN(newWait,2576);
    if (thisWait<17) {
      data.push_back(thisWait+0xaf);
    } else {
      int val=thisWait-17+0xe600;
      short bpos=backref.find(val);
      if (bpos>=0) {
        data.push_back(bpos);
      } else {
        backref.add(val);
        data.push_back(val>>8);
        data.push_back(val&0xff);
      }
    }
    newWait-=thisWait;
  }
}

std::vector<unsigned char> DivExportSNDH::runSubsong(int subsong, int& totalTicks) {
  // determine loop point
  int loopOrder=0;
  int loopRow=0;
  int loopEnd=0;
  e->walkSong(loopOrder,loopRow,loopEnd);
  logAppendf("loop point: %d %d",loopOrder,loopRow);
  e->warnings="";

  // prepare to write song data
  std::vector<unsigned char> data;
  std::vector<unsigned short> newData;
  BackRef backref;
  int timerHints[12];
  short regs[2][22];
  bool done=false;
  int totalWait=0;

  e->changeSong(subsong);
  e->curOrder=0;
  e->freelance=false;
  e->playing=false;
  e->extValuePresent=false;
  e->remainingLoops=-1;
  e->playSub(false);
  e->disCont[psgSys].dispatch->toggleRegisterDump(true);
  memset(regs,-1,sizeof(regs));
  memset(timerHints,0,sizeof(timerHints));

  DivConfig sysFlags = e->song.systemFlags[psgSys];

  while (!done) {
    if (e->nextTick(false,true) || !e->playing) {
      done=true;
      for (int i=0; i<e->song.systemLen; i++) {
        e->disCont[i].dispatch->getRegisterWrites().clear();
      }
      break;
    }
    newData.clear();
    // get register dumps
    std::vector<DivRegWrite>& writes=e->disCont[psgSys].dispatch->getRegisterWrites();
    for (DivRegWrite& write: writes) {
      if (write.addr>=0x10000 && write.addr<0x10009) {
        timerHints[write.addr&0xf]=write.val;
      } else {
        unsigned int addr=write.addr&0xf;
        short val=write.val&0xff;
        // same env shape value can retrig, do not dedup
        if (addr<13) {
          regs[0][addr]=val;
        } else {
          newData.push_back(0xd000|(addr<<8)|val);
        }
      }
    }
    writes.clear();
    // convert timer hints
    memset(timerHints+9,0,sizeof(timerHints[0])*3);
    for (int i=0; i<3; i++) {
      if (timerHints[i]==0) continue;
      if (timerHints[i+3]!=0 && timerHints[i+3]!=1) {
        if (regs[0][19+i]>=0) {
          // only disable timer
          regs[0][19+i]&=~7;
          // if disabling from PWM mode, force a volume write so it doesn't stuck at odd boundary
          if (regs[1][19+i]>=0 && (regs[1][19+i]&7)!=0 && (regs[0][19+i]&8)) {
            timerHints[i+9]=1;
          }
        }
      } else {
        int mode=timerHints[i+3]==0?(timerHints[i+6]<<1):1;
        // for sake of convenience, we only have to use the MFP converted values from dispatch
        int period = (long)timerHints[i]>>8;
        int mfpPeriod = period & 0xff;
        int mfpPrescaler = (period & 0xff00) >> 8;
        // we enforce a hard limit of 51.2kHz for the timers (1/4 prescaler, period 12)
        if (mfpPrescaler == 1) CLAMP_VAR(mfpPeriod, 12, 255)
        regs[0][16 + i] = mfpPeriod; // data register
        regs[0][19 + i] = mfpPrescaler | (mode << 3); // prescaler
      }
    }
    // deduplicate
    for (int i=0; i<22; i++) {
      if (regs[0][i]!=regs[1][i]) {
        // defer volume writes if disabling from PWM mode
        if (i>=8 && i<11 && timerHints[i-8+9]) continue;
        // if it's non-env volume writes, there's a special one-byte command for them
        if (i>=8 && i<11 && (regs[0][i]&0x10)==0) {
          newData.push_back((i<<4)+(regs[0][i]&0xf));
        } else {
          newData.push_back(0xd000+(i<<8)+regs[0][i]);
        }
      }
    }
    for (int i=8; i<11; i++) {
      if (timerHints[i-8+9]) {
        if ((regs[0][i]&0x10)==0) {
          newData.push_back((i<<4)+(regs[0][i]&0xf));
        } else {
          newData.push_back(0xd000+(i<<8)+regs[0][i]);
        }
      }
    }
    // write wait
    if (!newData.empty()) {
      writeWait(data,backref,totalWait);
    }
    // write commands
    for (unsigned short i: newData) {
      if (i<0x100) {
        data.push_back(i);
      } else {
        short bpos=backref.find(i);
        if (bpos>=0) {
          data.push_back(bpos);
        } else {
          backref.add(i);
          data.push_back(i>>8);
          data.push_back(i&0xff);
        }
      }
    }
    // done, move all regs to last
    memcpy(regs[1],regs[0],sizeof(regs[0]));

    totalWait+=e->cycles;
    totalTicks+=e->cycles;
  }
  writeWait(data,backref,totalWait);
  data.push_back(0xff);

  // end of song
  e->remainingLoops=-1;
  e->playing=false;
  e->freelance=false;
  e->extValuePresent=false;
  return data;
}

void DivExportSNDH::run() {
  // config
  int defaultSubsong=conf.getInt("defaultSubsong",MIN(e->getCurrentSubSong(),98));
  int tickRate=conf.getInt("tickRate",CLAMP(e->curSubSong->hz,1,200));
  // bool loop=conf.getBool("loop",true);
  bool packed=conf.getBool("packed",true);
  int numSubsongs=MIN(e->song.subsong.size(),99);

  // find PSG's index
  psgSys=-1;
  for (int i=0; i<e->song.systemLen; i++) {
    if (e->song.system[i] == DIV_SYSTEM_AY8910) {
      psgSys=i;
      logAppendf("Found a PSG at chip id %d",i);
      break;
    }
  }
  if (psgSys<0) {
    logAppend("ERROR: No PSG systems for SNDH");
    failed=true;
    running=false;
    return;
  }
  DivConfig sysFlags = e->song.systemFlags[psgSys];
  if (sysFlags.getInt("clockSel",0)!=3 || sysFlags.getBool("halfClock",false)) {
    logAppend("Warning: clock rate is not 2MHz, playback pitch will be incorrect");
  }
  if (sysFlags.getInt("chipType",0)!=1 && sysFlags.getInt("chipType",0)!=4) {
    logAppend("Warning: PSG type is not YM2149");
  }
  if (sysFlags.getInt("timerScheme",1)!=1) {
    logAppend("Warning: timer scheme is not MFP, timer effects will sound different");
  }
  if (sysFlags.getInt("timerClock",1)!=1) {
    logAppend("Warning: timer clock is not 2.4576MHz, timer effect speed will be incorrect");
  }

  std::vector<std::vector<unsigned char>> data;
  std::vector<int> seconds;
  e->stop();
  e->repeatPattern=false;
  e->synchronizedSoft([&]() {
    double origRate = e->got.rate;
    e->got.rate=tickRate;

    for (int i=0; i<numSubsongs; i++) {
      int ticks=0;
      logAppendf("rendering subsong %d...",i+1);
      progress[0].name=fmt::format("Subsong {}/{}",i+1,numSubsongs);
      progress[0].amount=(float)i/numSubsongs;
      data.push_back(runSubsong(i,ticks));
      seconds.push_back((ticks+tickRate-1)/tickRate);
    }

    // done
    e->got.rate=origRate;
    e->disCont[psgSys].dispatch->toggleRegisterDump(false);
  });
  progress[0].amount=0.99f;

  logAppend("writing data...");

  SafeWriter* w = new SafeWriter;
  w->init(); 
  
  // SNDH header
  w->writeI(0); // will be written later
  w->writeI(0);
  w->writeI(0);
  w->writeI(0x48444e53); // SNDH ident
  writeTextTag(w,"TITL",e->song.name);
  writeTextTag(w,"COMM",e->song.author);
  writeTextTag(w,"RIPP",e->song.category);
  writeTextTag(w,"CONV","Furnace (chiptune tracker)");
  w->writeString(fmt::format("TC{:03d}",tickRate),false);
  w->writeString(fmt::format("##{:02d}",numSubsongs),false);
  w->writeString(fmt::format("!#{:02d}",defaultSubsong+1),false);
  if (w->tell()&1) {
    w->writeC(0);
  }
  size_t snPos=w->tell();
  unsigned short snPoss[99];
  w->write("!#SN",4);
  for (int i=0; i<numSubsongs; i++) {
    w->writeS(0); // will be written later
  }
  for (int i=0; i<numSubsongs; i++) {
    snPoss[i]=w->tell()-snPos;
    w->writeString(e->song.subsong[i]->name,false);
  }
  if (w->tell()&1) {
    w->writeC(0);
  }
  size_t timePos=w->tell();
  w->seek(snPos+4,SEEK_SET);
  for (int i=0; i<numSubsongs; i++) {
    w->writeS_BE(snPoss[i]);
  }
  w->seek(timePos,SEEK_SET);
  w->write("TIME",4);
  for (int i=0; i<numSubsongs; i++) {
    w->writeS_BE(seconds[i]);
  }
  w->writeI(0x534e4448); // HDNS ident

  // player code
  while ((w->tell()&3)!=0) {
    w->writeC(0);
  }
  size_t playerPos=w->tell();
  w->seek(0,SEEK_SET);
  w->writeI_BE(0x60000000+playerPos-2); // bra.w init
  w->writeI_BE(0x60000000+playerPos-2); // bra.w exit
  w->writeI_BE(0x60000000+playerPos-2); // bra.w play
  w->seek(playerPos,SEEK_SET);
  int padLength = (int)player_sndh_regdump[0] << 8 | player_sndh_regdump[1];
  w->write(player_sndh_regdump+2,player_sndh_regdump_len-2);
  for (int i=0; i<padLength; i++) {
    w->writeC(0);
  }
  // song data
  size_t dataPos=w->tell();
  unsigned int dataPoss[99];
  for (int i=0; i<numSubsongs; i++) {
    w->writeI_BE(0); // will be written later
  }
  for (int i=0; i<numSubsongs; i++) {
    dataPoss[i]=w->tell()-dataPos;
    w->write(data[i].data(),data[i].size());
  }
  w->seek(dataPos,SEEK_SET);
  for (int i=0; i<numSubsongs; i++) {
    w->writeI_BE(dataPoss[i]); // will be written later
  }

  int len = w->size();
  if (packed) {
    logAppend("compressing output...");
    unsigned char* source;
    unsigned char* dest;
    source = w->getFinalBuf();
    dest = (unsigned char*)ice_crunch((char*)source,len,1);

    size_t iceLen = ice_crunched_length((char*)dest);
    SafeWriter* o = new SafeWriter;
    o->init();
    o->write(dest, iceLen);
    output.push_back(DivROMExportOutput("export.sndh", o));
    logAppendf("compressed from %d bytes to %d bytes", len, iceLen);
  } else {
    output.push_back(DivROMExportOutput("export.sndh", w));
  }

  progress[0].amount=1.0f;
  
  logAppend("finished!");

  running=false;
}

bool DivExportSNDH::go(DivEngine* eng) {
  progress[0].name="Subsong";
  progress[0].amount=0.0f;

  e=eng;
  running=true;
  failed=false;
  mustAbort=false;
  exportThread=new std::thread(&DivExportSNDH::run,this);
  return true;
}

void DivExportSNDH::wait() {
  if (exportThread!=NULL) {
    logV("waiting for export thread...");
    exportThread->join();
    delete exportThread;
  }
}

void DivExportSNDH::abort() {
  mustAbort=true;
  wait();
}

bool DivExportSNDH::isRunning() {
  return running;
}

bool DivExportSNDH::hasFailed() {
  return failed;
}

DivROMExportProgress DivExportSNDH::getProgress(int index) {
  if (index<0 || index>1) return progress[1];
  return progress[index];
}

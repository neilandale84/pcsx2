/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"
#include "Common.h"

#include "GS.h"
#include "Gif_Unit.h"
#include "Vif_Dma.h"

#include "iR5900.h"

// A three-way toggle used to determine if the GIF is stalling (transferring) or done (finished).
// Should be a gifstate_t rather then int, but I don't feel like possibly interfering with savestates right now.


__aligned16 GIF_Fifo gif_fifo;
__aligned16 gifStruct gif;

static __fi void GifDMAInt(int cycles) {
	if (dmacRegs.ctrl.MFD == MFD_GIF) {
		if (!(cpuRegs.interrupt & (1 << DMAC_MFIFO_GIF)) || cpuRegs.eCycle[DMAC_MFIFO_GIF] < (u32)cycles)
		{
			CPU_INT(DMAC_MFIFO_GIF, cycles);
		}
	} else if (!(cpuRegs.interrupt & (1 << DMAC_GIF)) || cpuRegs.eCycle[DMAC_GIF] < (u32)cycles)
	{
		CPU_INT(DMAC_GIF, cycles);
	}
}
__fi void clearFIFOstuff(bool full) {
		CSRreg.FIFO = full ? CSR_FIFO_FULL : CSR_FIFO_EMPTY;
}

//I suspect this is GS side which should really be handled by the GS plugin which also doesn't current have a fifo, but we can guess from our fifo
static __fi void CalculateFIFOCSR() {
	if (gifRegs.stat.FQC >= 15) {
		CSRreg.FIFO = CSR_FIFO_FULL;
	}
	else if (gifRegs.stat.FQC == 0) {
		CSRreg.FIFO = CSR_FIFO_EMPTY;
	}
	else {
		CSRreg.FIFO = CSR_FIFO_NORMAL;
	}
}

void GIF_Fifo::init()
{
	readpos = 0;
	writepos = 0;
	memzero(data);
	memzero(readdata);
	gifRegs.stat.FQC = 0;
	CSRreg.FIFO = CSR_FIFO_EMPTY;
	gif.gifstate = GIF_STATE_READY;
	gif.gspath3done = false;

	gif.gscycles = 0;
	gif.prevcycles = 0;
	gif.mfifocycles = 0;
	gif.gifqwc = 0;

}


int GIF_Fifo::write(u32* pMem, int size)
{
	if (gifRegs.stat.FQC == 16) {
		//log_cb(RETRO_LOG_DEBUG, "Full\n");
		return 0;
	}
	int transsize;
	int firsttrans = std::min(size, 16 - (int)gifRegs.stat.FQC);

	gifRegs.stat.FQC += firsttrans;
	transsize = firsttrans;
	
	
	while (transsize-- > 0)
	{
		CopyQWC(&data[writepos], pMem);
		writepos = (writepos + 4) & 63;
		pMem += 4;
	}
	
	CalculateFIFOCSR();
	return firsttrans;
}

int GIF_Fifo::read(bool calledFromDMA)
{

	if (!gifUnit.CanDoPath3() || gifRegs.stat.FQC == 0)
	{
		//log_cb(RETRO_LOG_DEBUG, "Path3 not masked\n");
		if (gifch.chcr.STR == true && !(cpuRegs.interrupt & (1 << DMAC_GIF)) && calledFromDMA == false) {
			GifDMAInt(16);
		}
		//log_cb(RETRO_LOG_DEBUG, "P3 Masked\n");
		return 0;
	}

	int valueWritePos = 0;
	uint sizeRead;
	uint fifoSize = gifRegs.stat.FQC;
	int oldReadPos = readpos;

	while (gifRegs.stat.FQC) {
		CopyQWC(&readdata[valueWritePos], &data[readpos]);
		readpos = (readpos + 4) & 63;
		valueWritePos = (valueWritePos + 4) & 63;
		gifRegs.stat.FQC--;
	}

	sizeRead = gifUnit.TransferGSPacketData(GIF_TRANS_DMA, (u8*)&readdata[0], fifoSize * 16) / 16; //returns the size actually read

	if (sizeRead < fifoSize) {
		readpos = (oldReadPos + (sizeRead * 4)) & 63; //if we read less than what was in the fifo, move the read position back
		gifRegs.stat.FQC = fifoSize - sizeRead;
	}
		
	if (calledFromDMA == false) {
		GifDMAInt(sizeRead * BIAS);
	}

	CalculateFIFOCSR();
	return gifRegs.stat.FQC;
}

void incGifChAddr(u32 qwc) {
	if (gifch.chcr.STR) {
		gifch.madr += qwc * 16;
		gifch.qwc  -= qwc;
		hwDmacSrcTadrInc(gifch);
	}
#if 0
	else
		log_cb(RETRO_LOG_DEBUG, "incGifAddr() Error!\n");
#endif
}

__fi void gifCheckPathStatus() {

	if (gifRegs.stat.APATH == 3)
	{
		gifRegs.stat.APATH = 0;
		gifRegs.stat.OPH = 0;
		if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE || gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_WAIT)
		{
			if (gifUnit.checkPaths(1, 1, 0)) gifUnit.Execute(false, true);
		}
	}

	//Required for Path3 Masking timing!
	if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_WAIT)
		gifUnit.gifPath[GIF_PATH_3].state = GIF_PATH_IDLE;
}

__fi void gifInterrupt()
{
	GIF_LOG("gifInterrupt caught qwc=%d fifo=%d apath=%d oph=%d state=%d!", gifch.qwc, gifRegs.stat.FQC, gifRegs.stat.APATH, gifRegs.stat.OPH, gifUnit.gifPath[GIF_PATH_3].state);
	gifCheckPathStatus();

	if(gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE)
	{
		if(vif1Regs.stat.VGW)
		{
			//Check if VIF is in a cycle or is currently "idle" waiting for GIF to come back.
			if(!(cpuRegs.interrupt & (1<<DMAC_VIF1)))
				CPU_INT(DMAC_VIF1, 1);

			//Make sure it loops if the GIF packet is empty to prepare for the next packet
			//or end if it was the end of a packet.
			//This must trigger after VIF retriggers as VIf might instantly mask Path3
			if (!gifUnit.Path3Masked() || gifch.qwc == 0) {
				GifDMAInt(16);
			}
			return;
		}
		
	}

	if (dmacRegs.ctrl.MFD == MFD_GIF) { // GIF MFIFO
		//log_cb(RETRO_LOG_INFO, "GIF MFIFO\n");
		gifMFIFOInterrupt();
		return;
	}	

	if (CHECK_GIFFIFOHACK) {

		if (int amtRead = gif_fifo.read(true)) {

			if (!gifUnit.Path3Masked() || gifRegs.stat.FQC < 16) {
				GifDMAInt(amtRead * BIAS);
				return;
			}
		}
		else {

			if (!gifUnit.CanDoPath3() && gifRegs.stat.FQC == 16)
			{
				if (gifch.qwc > 0 || gif.gspath3done == false) {
					if (!gifUnit.Path3Masked()) {
						GifDMAInt(128);
					}
					return;
				}
			}
		}
	}
	

	if (gifUnit.gsSIGNAL.queued) {
		GIF_LOG("Path 3 Paused");
		GifDMAInt(128);
		return;
	}

	gifCheckPathStatus();

	//Double check as we might have read the fifo as it's ending the DMA
	if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE)
	{
		if (vif1Regs.stat.VGW)
		{
			//Check if VIF is in a cycle or is currently "idle" waiting for GIF to come back.
			if (!(cpuRegs.interrupt & (1 << DMAC_VIF1))) {
				CPU_INT(DMAC_VIF1, 1);
			}
		}
	}
	
	if (!(gifch.chcr.STR)) return;

	if ((gifch.qwc > 0) || (!gif.gspath3done)) {
		if (!dmacRegs.ctrl.DMAE) {
			log_cb(RETRO_LOG_WARN, "gs dma masked, re-scheduling...\n");
			// re-raise the int shortly in the future
			GifDMAInt( 64 );
			return;
		}
		GIFdma();
		
		return;
	}

	
	
	if (!CHECK_GIFFIFOHACK)
	{
		gifRegs.stat.FQC = 0;
		clearFIFOstuff(false);
	}
	gif.gscycles = 0;
	gifch.chcr.STR	 = false;

	hwDmacIrq(DMAC_GIF);
	GIF_LOG("GIF DMA End QWC in fifo %x APATH = %x OPH = %x state = %x", gifRegs.stat.FQC, gifRegs.stat.APATH, gifRegs.stat.OPH, gifUnit.gifPath[GIF_PATH_3].state);
}

static u32 WRITERING_DMA(u32 *pMem, u32 qwc) {
	if (gifRegs.stat.IMT)
	{
		//Splitting by 8qw can be really slow, so on bigger packets be less picky.
		//Some games like Wallace & Gromit like smaller packets to be split correctly, hopefully with little impact on speed.
		//68 works for W&G but 128 is more of a safe point.
		if (qwc > 128)
			qwc = std::min(qwc, 1024u);
		else
			qwc = std::min(qwc, 8u);
	}
	uint size;
	if (CHECK_GIFFIFOHACK) {
		size = gif_fifo.write(pMem, qwc);
	}
	else {
		size = gifUnit.TransferGSPacketData(GIF_TRANS_DMA, (u8*)pMem, qwc * 16) / 16;
	}
	incGifChAddr(size);
	return size;
}

int  _GIFchain()
{
	tDMA_TAG *pMem;

	pMem = dmaGetAddr(gifch.madr, false);
	if (pMem == NULL) {
		//must increment madr and clear qwc, else it loops
		gifch.madr += gifch.qwc * 16;
		gifch.qwc = 0;
		log_cb(RETRO_LOG_WARN, "Hackfix - NULL GIFchain\n");
		return -1;
	}

	return WRITERING_DMA((u32*)pMem, gifch.qwc);
}

static __fi void GIFchain() {
	// qwc check now done outside this function
	// Voodoocycles
	// >> 2 so Drakan and Tekken 5 don't mess up in some PATH3 transfer. Cycles to interrupt were getting huge..
	/*if (gifch.qwc)*/
	gif.gscycles+= _GIFchain() * BIAS; /* guessing */
}

static __fi bool checkTieBit(tDMA_TAG* &ptag)
{
	if (gifch.chcr.TIE && ptag->IRQ) {
		GIF_LOG("dmaIrq Set");
		gif.gspath3done = true;
		return true;
	}
	return false;
}

static __fi tDMA_TAG* ReadTag()
{
	tDMA_TAG* ptag = dmaGetAddr(gifch.tadr, false);  //Set memory pointer to TADR

	if (!(gifch.transfer("Gif", ptag))) return NULL;

	gifch.madr = ptag[1]._u32;	//MADR = ADDR field + SPR
	gif.gscycles += 2;				// Add 1 cycles from the QW read for the tag

	gif.gspath3done = hwDmacSrcChainWithStack(gifch, ptag->ID);
	return ptag;
}

static __fi tDMA_TAG* ReadTag2()
{
	tDMA_TAG* ptag = dmaGetAddr(gifch.tadr, false);  //Set memory pointer to TADR

	gifch.unsafeTransfer(ptag);
	gifch.madr = ptag[1]._u32;

	gif.gspath3done = hwDmacSrcChainWithStack(gifch, ptag->ID);
	return ptag;
}

bool CheckPaths() {
	// Can't do Path 3, so try dma again later...
	if (!CHECK_GIFFIFOHACK) {
		if (!gifUnit.CanDoPath3()) {
			if (!gifUnit.Path3Masked())
			{
				GIF_LOG("Path3 stalled");
				GifDMAInt(128);
			}
			return false;
		}
	}
	return true;
}

void GIFdma()
{
	while (gifch.qwc > 0 || !gif.gspath3done) {
		tDMA_TAG* ptag;
		gif.gscycles = gif.prevcycles;

		if (gifRegs.ctrl.PSE) { // temporarily stop
			log_cb(RETRO_LOG_INFO, "Gif dma temp paused? (non MFIFO GIF)\n");
			GifDMAInt(16);
			return;
		}

		if ((dmacRegs.ctrl.STD == STD_GIF) && (gif.prevcycles != 0)) {
			//log_cb(RETRO_LOG_INFO, "GS Stall Control Source = %x, Drain = %x\n MADR = %x, STADR = %x\n", (psHu32(0xe000) >> 4) & 0x3, (psHu32(0xe000) >> 6) & 0x3, gifch.madr, psHu32(DMAC_STADR));
			if ((gifch.madr + (gifch.qwc * 16)) > dmacRegs.stadr.ADDR) {
				GifDMAInt(4);
				gif.gscycles = 0;
				return;
			}
			gif.prevcycles = 0;
			gifch.qwc = 0;
		}

		if ((gifch.chcr.MOD == CHAIN_MODE) && (!gif.gspath3done) && gifch.qwc == 0) // Chain Mode
		{
			ptag = ReadTag();
			if (ptag == NULL) return;
			//log_cb(RETRO_LOG_DEBUG, "GIF Reading Tag MSK = %x\n", vif1Regs.mskpath3);
			GIF_LOG("gifdmaChain %8.8x_%8.8x size=%d, id=%d, addr=%lx tadr=%lx", ptag[1]._u32, ptag[0]._u32, gifch.qwc, ptag->ID, gifch.madr, gifch.tadr);
			if (!CHECK_GIFFIFOHACK)gifRegs.stat.FQC = std::min((u32)0x10, gifch.qwc);// FQC=31, hack ;) (for values of 31 that equal 16) [ used to be 0xE00; // APATH=3]
			if (dmacRegs.ctrl.STD == STD_GIF)
			{
				// there are still bugs, need to also check if gifch.madr +16*qwc >= stadr, if not, stall
				if ((ptag->ID == TAG_REFS) && ((gifch.madr + (gifch.qwc * 16)) > dmacRegs.stadr.ADDR))
				{
					// stalled.
					// We really need to test this. Pay attention to prevcycles, as it used to trigger GIFchains in the code above. (rama)
					//log_cb(RETRO_LOG_DEBUG, "GS Stall Control start Source = %x, Drain = %x\n MADR = %x, STADR = %x\n", (psHu32(0xe000) >> 4) & 0x3, (psHu32(0xe000) >> 6) & 0x3,gifch.madr, psHu32(DMAC_STADR));
					gif.prevcycles = gif.gscycles;
					gifch.tadr -= 16;
					gifch.qwc = 0;
					hwDmacIrq(DMAC_STALL_SIS);
					GifDMAInt(128);
					gif.gscycles = 0;
					return;
				}
			}

			checkTieBit(ptag);
		}
#ifndef NDEBUG
		else if (dmacRegs.ctrl.STD == STD_GIF && gifch.chcr.MOD == NORMAL_MODE)
		{
			log_cb(RETRO_LOG_INFO, "GIF DMA Stall in Normal mode not implemented - Report which game to PCSX2 Team\n");
		}
#endif


		if (!CHECK_GIFFIFOHACK) {
			gifRegs.stat.FQC = std::min((u32)0x10, gifch.qwc);// FQC=31, hack ;) (for values of 31 that equal 16) [ used to be 0xE00; // APATH=3]
			clearFIFOstuff(true);
		}

		// Transfer Dn_QWC from Dn_MADR to GIF
		if (gifch.qwc > 0) // Normal Mode
		{
			if (CheckPaths() == false) return;

			GIFchain();	//Transfers the data set by the switch
#if 0
			if (gscycles < 8)
				log_cb(RETRO_LOG_DEBUG, "GSCycles = %d\n", gscycles);
#endif
			GifDMAInt(gif.gscycles);
			return;
		}
	}

	//QWC == 0 && gspath3done == true - End of DMA
	gif.prevcycles = 0;
#if 0
	if (gscycles < 8)
		log_cb(RETRO_LOG_ERROR, "1 GSCycles = %d\n", gscycles);
#endif
	GifDMAInt(16);
}

void dmaGIF()
{
	 //We used to add wait time for the buffer to fill here, fixing some timing problems in path 3 masking
	//It takes the time of 24 QW for the BUS to become ready - The Punisher And Streetball
	//log_cb(RETRO_LOG_DEBUG, "dmaGIFstart chcr = %lx, madr = %lx, qwc  = %lx\n tadr = %lx, asr0 = %lx, asr1 = %lx\n", gifch.chcr._u32, gifch.madr, gifch.qwc, gifch.tadr, gifch.asr0, gifch.asr1);

	gif.gspath3done = false; // For some reason this doesn't clear? So when the system starts the thread, we will clear it :)

	if (!CHECK_GIFFIFOHACK) {
		gifRegs.stat.FQC |= 0x10; // hack ;)
		clearFIFOstuff(true);
	}

	if (gifch.chcr.MOD == NORMAL_MODE) { //Else it really is a normal transfer and we want to quit, else it gets confused with chains
		gif.gspath3done = true;
	}


	if(gifch.chcr.MOD == CHAIN_MODE && gifch.qwc > 0) {
		//log_cb(RETRO_LOG_DEBUG, "GIF QWC on Chain %s\n" + gifch.chcr.desc().c_str());
		if ((gifch.chcr.tag().ID == TAG_REFE) || (gifch.chcr.tag().ID == TAG_END) || (gifch.chcr.tag().IRQ && gifch.chcr.TIE)) {
			gif.gspath3done = true;
		}
	}

	gifInterrupt();
}

static u32 QWCinGIFMFIFO(u32 DrainADDR)
{
	u32 ret;

	SPR_LOG("GIF MFIFO Requesting %x QWC from the MFIFO Base %x, SPR MADR %x Drain %x", gifch.qwc, dmacRegs.rbor.ADDR, spr0ch.madr, DrainADDR);
	//Calculate what we have in the fifo.
	if (DrainADDR <= spr0ch.madr) {
		//Drain is below the write position, calculate the difference between them
		ret = (spr0ch.madr - DrainADDR) >> 4;
	}
	else {
		u32 limit = dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16;
		//Drain is higher than SPR so it has looped round, 
		//calculate from base to the SPR tag addr and what is left in the top of the ring
		ret = ((spr0ch.madr - dmacRegs.rbor.ADDR) + (limit - DrainADDR)) >> 4;
	}
	if (ret == 0) 
		gif.gifstate |= GIF_STATE_EMPTY;

	SPR_LOG("%x Available of the %x requested", ret, gifch.qwc);
	return ret;
}

static __fi bool mfifoGIFrbTransfer()
{
	u32 qwc = std::min(QWCinGIFMFIFO(gifch.madr), gifch.qwc);
#if 0
	if (qwc == 0)
		log_cb(RETRO_LOG_DEBUG, "GIF FIFO EMPTY before transfer (how?)\n");
#endif

	u8* src = (u8*)PSM(gifch.madr);
	if (src == NULL) return false;

	u32 MFIFOUntilEnd = ((dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16) - gifch.madr) >> 4;
	bool needWrap = MFIFOUntilEnd < qwc;
	u32 firstTransQWC = needWrap ? MFIFOUntilEnd : qwc;
	u32 transferred;

	if (!CHECK_GIFFIFOHACK) 
	{
		transferred = gifUnit.TransferGSPacketData(GIF_TRANS_DMA, src, firstTransQWC * 16) / 16; // First part
	}
	else 
	{
		transferred = gif_fifo.write((u32*)src, firstTransQWC);
	}

	incGifChAddr(transferred);

	gifch.madr = dmacRegs.rbor.ADDR + (gifch.madr & dmacRegs.rbsr.RMSK);
	gifch.tadr = dmacRegs.rbor.ADDR + (gifch.tadr & dmacRegs.rbsr.RMSK);

	if (needWrap && transferred == MFIFOUntilEnd) 
	{ // Need to do second transfer to wrap around
		u32 transferred2;
		uint secondTransQWC = qwc - MFIFOUntilEnd;

		src = (u8*)PSM(dmacRegs.rbor.ADDR);
		if (src == NULL) return false;

		if (!CHECK_GIFFIFOHACK) {
			transferred2 = gifUnit.TransferGSPacketData(GIF_TRANS_DMA, src, secondTransQWC * 16) / 16; // Second part
		}
		else {
			transferred2 = gif_fifo.write((u32*)src, secondTransQWC);
		}

		incGifChAddr(transferred2);
		gif.mfifocycles += (transferred2 + transferred) * 2; // guessing
	}
	else 
	{
		gif.mfifocycles += transferred * 2; // guessing
	}

	return true;
}

static __fi bool mfifoGIFchain()
{
	/* Is QWC = 0? if so there is nothing to transfer */
	if (gifch.qwc == 0) return true;

	if ((gifch.madr & ~dmacRegs.rbsr.RMSK) == dmacRegs.rbor.ADDR)
	{
		bool ret = true;

		if (QWCinGIFMFIFO(gifch.madr) == 0) {
			SPR_LOG("GIF FIFO EMPTY before transfer");
			gif.gifstate = GIF_STATE_EMPTY;
			gif.mfifocycles += 4;
			if (CHECK_GIFFIFOHACK)
				GifDMAInt(128);
			return true;
		}

		if (!mfifoGIFrbTransfer()) ret = false;

		//This ends up being done more often but it's safer :P
		//Make sure we wrap the addresses, dont want it being stuck outside the ring when reading from the ring!
		gifch.madr = dmacRegs.rbor.ADDR + (gifch.madr & dmacRegs.rbsr.RMSK);
		gifch.tadr = gifch.madr;

		return ret;
	}
	else {
		int mfifoqwc;
		SPR_LOG("Non-MFIFO Location transfer doing %x Total QWC", gifch.qwc);
		tDMA_TAG *pMem = dmaGetAddr(gifch.madr, false);
		if (pMem == NULL) return false;

		mfifoqwc = WRITERING_DMA((u32*)pMem, gifch.qwc);
		gif.mfifocycles += (mfifoqwc) * 2; /* guessing */
	}

	return true;
}

static u32 qwctag(u32 mask) {
	return (dmacRegs.rbor.ADDR + (mask & dmacRegs.rbsr.RMSK));
}

void mfifoGifMaskMem(int id)
{
	switch (id) {
		//These five transfer data following the tag, need to check its within the buffer (Front Mission 4)
		case TAG_CNT:
		case TAG_NEXT:
		case TAG_CALL: 
		case TAG_RET:
		case TAG_END:
			if(gifch.madr < dmacRegs.rbor.ADDR) //probably not needed but we will check anyway.
			{
				SPR_LOG("GIF MFIFO MADR below bottom of ring buffer, wrapping GIF MADR = %x Ring Bottom %x", gifch.madr, dmacRegs.rbor.ADDR);
				gifch.madr = qwctag(gifch.madr);
			} else
			if(gifch.madr > (dmacRegs.rbor.ADDR + (u32)dmacRegs.rbsr.RMSK)) //Usual scenario is the tag is near the end (Front Mission 4)
			{
				SPR_LOG("GIF MFIFO MADR outside top of ring buffer, wrapping GIF MADR = %x Ring Top %x", gifch.madr, (dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK)+16);
				gifch.madr = qwctag(gifch.madr);
			}
			break;
		default:
			//Do nothing as the MADR could be outside
			break;
	}
}

void mfifoGIFtransfer()
{
	tDMA_TAG *ptag;
	gif.mfifocycles = 0;
	

	if (gifRegs.ctrl.PSE) { // temporarily stop
		log_cb(RETRO_LOG_INFO, "Gif dma temp paused?\n");
		CPU_INT(DMAC_MFIFO_GIF, 16);
		return;
	}

	if (gifch.qwc == 0) {
		gifch.tadr = qwctag(gifch.tadr);

		if (QWCinGIFMFIFO(gifch.tadr) == 0) {
			SPR_LOG("GIF FIFO EMPTY before tag read");
			gif.gifstate = GIF_STATE_EMPTY;
			GifDMAInt(4);
			if (CHECK_GIFFIFOHACK)
				GifDMAInt(128);
			return;
		}

		ptag = dmaGetAddr(gifch.tadr, false);
		gifch.unsafeTransfer(ptag);
		gifch.madr = ptag[1]._u32;

		gif.mfifocycles += 2;

		GIF_LOG("dmaChain %8.8x_%8.8x size=%d, id=%d, madr=%lx, tadr=%lx mfifo qwc = %x spr0 madr = %x",
				ptag[1]._u32, ptag[0]._u32, gifch.qwc, ptag->ID, gifch.madr, gifch.tadr, gif.gifqwc, spr0ch.madr);

		gif.gspath3done = hwDmacSrcChainWithStack(gifch, ptag->ID);

#ifndef NDEBUG
		if (dmacRegs.ctrl.STD == STD_GIF && (ptag->ID == TAG_REFS))
		{
			log_cb(RETRO_LOG_INFO, "GIF MFIFO DMA Stall not implemented - Report which game to PCSX2 Team\n");
		}
#endif
		mfifoGifMaskMem(ptag->ID);

		gifch.tadr = qwctag(gifch.tadr);

		if ((gifch.chcr.TIE) && (ptag->IRQ)) {
			SPR_LOG("dmaIrq Set");
			gif.gspath3done = true;
		}
	 }

	if (!mfifoGIFchain()) {
#ifndef NDEBUG
		log_cb(RETRO_LOG_INFO, "mfifoGIF dmaChain error size=%d, madr=%lx, tadr=%lx\n", gifch.qwc, gifch.madr, gifch.tadr);
#endif
		gif.gspath3done = true;
		gifch.qwc = 0; //Sanity
	}

	GifDMAInt(std::max(gif.mfifocycles, (u32)4));

	SPR_LOG("mfifoGIFtransfer end %x madr %x, tadr %x", gifch.chcr._u32, gifch.madr, gifch.tadr);
}

void gifMFIFOInterrupt()
{
    GIF_LOG("gifMFIFOInterrupt");
	gif.mfifocycles = 0;

	if (dmacRegs.ctrl.MFD != MFD_GIF) { // GIF not in MFIFO anymore, come out.
#ifndef NDEBUG
		log_cb(RETRO_LOG_DEBUG, "GIF Leaving MFIFO - Report if any errors\n");
#endif
		gifInterrupt();
		return;
	}

	gifCheckPathStatus();

	if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE)
	{
		if (vif1Regs.stat.VGW)
		{
			//Check if VIF is in a cycle or is currently "idle" waiting for GIF to come back.
			if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
				CPU_INT(DMAC_VIF1, 1);

			//Make sure it loops if the GIF packet is empty to prepare for the next packet
			//or end if it was the end of a packet.
			//This must trigger after VIF retriggers as VIf might instantly mask Path3
			if (!gifUnit.Path3Masked() || gifch.qwc == 0) {
				GifDMAInt(16);
			}
			return;
		}

	}

	if (gifUnit.gsSIGNAL.queued) {
		GifDMAInt(128);
		return;
	}

	if (CHECK_GIFFIFOHACK) 
	{
		if (int amtRead = gif_fifo.read(true))
		{
			if (!gifUnit.Path3Masked() || gifRegs.stat.FQC < 16) {
				GifDMAInt(amtRead * BIAS);
				return;
			}
		}
		else {
			if (!gifUnit.CanDoPath3() && gifRegs.stat.FQC == 16)
			{
				if (gifch.qwc > 0 || gif.gspath3done == false) {
					if (!gifUnit.Path3Masked()) {
						GifDMAInt(128);
					}
					return;
				}
			}
		}
	}
	gifCheckPathStatus();

	if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE)
	{
		if (vif1Regs.stat.VGW)
		{
			//Check if VIF is in a cycle or is currently "idle" waiting for GIF to come back.
			if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
				CPU_INT(DMAC_VIF1, 1);

			//Make sure it loops if the GIF packet is empty to prepare for the next packet
			//or end if it was the end of a packet.
			//This must trigger after VIF retriggers as VIf might instantly mask Path3
			if (!gifUnit.Path3Masked() || gifch.qwc == 0) {
				GifDMAInt(16);
			}
			return;
		}
	}

	if (!gifch.chcr.STR) {
#ifndef NDEBUG
		log_cb(RETRO_LOG_INFO, "WTF GIFMFIFO\n");
#endif
		cpuRegs.interrupt &= ~(1 << 11);
		return;
	}

	if ((gif.gifstate & GIF_STATE_EMPTY)) {
		FireMFIFOEmpty();
		if (CHECK_GIFFIFOHACK)
			GifDMAInt(128);
		return;
	}

	if (gifch.qwc > 0 || !gif.gspath3done) {

		if (!CheckPaths()) return;
		mfifoGIFtransfer();
		return;
	}

	if (!CHECK_GIFFIFOHACK)
	{
		gifRegs.stat.FQC = 0;
		clearFIFOstuff(false);
	}
	
	if (spr0ch.madr == gifch.tadr) {
		FireMFIFOEmpty();
	}

	gif.gscycles = 0;
	
	gifch.chcr.STR = false;
	gif.gifstate = GIF_STATE_READY;
	hwDmacIrq(DMAC_GIF);
	DMA_LOG("GIF MFIFO DMA End");
}

void SaveStateBase::gifDmaFreeze() {
	// Note: mfifocycles is not a persistent var, so no need to save it here.
	FreezeTag("GIFdma");
	Freeze(gif.gifstate);
	Freeze(gif.gifqwc);
	Freeze(gif.gspath3done);
	Freeze(gif.gscycles);
	Freeze(gif_fifo);
}

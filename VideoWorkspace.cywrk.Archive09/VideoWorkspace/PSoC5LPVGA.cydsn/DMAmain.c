/* ========================================
 *
 * Copyright YOUR COMPANY, THE YEAR
 * All Rights Reserved
 * UNPUBLISHED, LICENSED SOFTWARE.
 *
 * CONFIDENTIAL AND PROPRIETARY INFORMATION
 * WHICH IS THE PROPERTY OF your company.
 *
 * ========================================
*/
#include <project.h>

// Get the resolution from the Video Controller instance.
#define VGA_RES_X VideoCtrl_1_H_RES
#define VGA_RES_Y VideoCtrl_1_V_RES
// Our buffer will be one bit per pixel so we only need 1/8th for our horizontal dimension.
#define VGA_X_FACTOR 8
// We don't have enough memory so we are going to duplicate the vertical lines
// to save on memory requirements.
#define VGA_Y_FACTOR 2
// This is our final dimmensions for our frame buffers.
#define VGA_X_BYTES ((VGA_RES_X)/VGA_X_FACTOR)
#define VGA_Y_BYTES (VGA_RES_Y/VGA_Y_FACTOR)
#define VGA_BUFF_SIZE (VGA_X_BYTES*VGA_Y_BYTES)

#define DMA_MEM_CPY 1
#if DMA_MEM_CPY
// Define how many bytes we are going to copy per transfer count
// 0-4095, since we want to do 4 bytes at a time use something divisable by 4.
#define MEM_TRANSFER_COUNT  4092
#define PARTIAL_TD_COUNT (VGA_BUFF_SIZE%MEM_TRANSFER_COUNT)
// Depending if the frame buffer is divisable by MEM_TRANSFER_COUNT allocate an extra TD or not.
// It's better to do this in the pre-processor so we don't waste SRAM for a variable to decide later nore the extra TD allocation.
// Remember we only have 64KB to play around and only half of those are in the code space.
#if PARTIAL_TD_COUNT
    // How mandy TDs are we going to need + 1 (i.e. 100 colums by 300 lines in 800x600 mode, 30,000/2,048 = 14.6484375)
    #define NUM_MEM_TDS (1+VGA_BUFF_SIZE/MEM_TRANSFER_COUNT)
#else
    // How mandy TDs are we going to need (i.e. 128 colums by 384 lines in 1024x768 mode, 49152/2,048 = 24)
    // That is if we had the frame buffer memory to cope with that resolution
    // but applicable to any 2048 multiple frame buffer if one exists.
    #define NUM_MEM_TDS (VGA_BUFF_SIZE/MEM_TRANSFER_COUNT)
#endif
// Add a second DMA channel and Transaction Descriptor for memory to memory transfers.
uint8 damMemCh, dmaMemTd[NUM_MEM_TDS];
// Flag to indicate mem_DMA transfer is complete
volatile uint8 flag_FrameRdyone = 0;
#endif

// Declare our DMA channel and our DMA Transaction Descriptor.
uint8 dmaCh, dmaTd;

// Set up a refresh signal so the CPU can refresh the DMA buffer.
volatile int refresh = 0;

//
// Define our frame buffers making sure the X dimension is continuous in memory.
//
// CPU frame, by default this will be on 0x1FFF8000 Code SRAM space (i.e. section .ram)
uint8 cframe[VGA_Y_BYTES][VGA_X_BYTES] __attribute__((aligned()));
// DMA frame, Declare the DMA video frame  to go in our new section .ram2 located at 0x2000000
uint8 dframe[VGA_Y_BYTES][VGA_X_BYTES] __attribute__ ((aligned(),section(".ram2")));


// ScanLine Interrupt
//
// This gets called everytime our DMA transfer is done, so we can setup the next line
// or if we are on the last line we will refresh the dma frame with the current cpu frame.
CY_ISR(ScanLine)
{
    // Get our line count from both status registers
    // LINE_CNT_HI holds the upper 2 bits
    // LINE_CNT_LO holds the lower 8 bits
    volatile uint16 line = ((LINE_CNT_HI_Status<<8))|LINE_CNT_LO_Status;

    // We don't want to change anything unless we are past line 0.
    if (line)
    {
        // Check if we are within the visible area.
        if (line < VGA_RES_Y)
        {
            // Update the next DMA transfer for the next line.
            // adusting the line by the Y skip factor.
            if ((line % VGA_Y_FACTOR) == 0)
            {
                CY_SET_REG16(CY_DMA_TDMEM_STRUCT_PTR[dmaTd].TD1, LO16((uint32) dframe[line / VGA_Y_FACTOR]));                    
            }
        }
        if ((line+1) == VGA_RES_Y)
        {
            // On the last line since we are going to enter vertical sync
            // Indicate the CPU that it's ok to refresh the screen.
            // this is implemented as a counter in case we want to wait more than one frame.
            refresh++;
        }
    }
}

#if DMA_MEM_CPY
CY_ISR(FrameRdy)
{
	// Set the flag to indicate that the current DMA memory to memory tranfer is complete
	flag_FrameRdyone = 0x01;
}
#endif

int main()
{
    //
    // DMA setup
    //
    // Alocate a transaction descriptor.
    dmaTd = CyDmaTdAllocate();
    // Initialize the DMA channel to transfer from the dframe base address to the control base address.
    // This indicates the high 16 bit address that will apply to the low addresses set on the TD.
    dmaCh = DMA_DmaInitialize(1, 0, HI16((uint32) dframe), HI16(CYDEV_PERIPH_BASE));
    // Configure the transaction descriptor for the first transfer.
    // Transder VGA_X_BYTES with auto increment and signalling the end of the transfer.
    // use the single transaction descriptor as our next TD as well.
    CyDmaTdSetConfiguration(dmaTd, VGA_X_BYTES, dmaTd, DMA__TD_TERMOUT_EN | TD_INC_SRC_ADR);
    // Set the destination address to be our DMA_OUT control register in the schematic.
    CyDmaTdSetAddress(dmaTd, LO16((uint32) dframe[0]), LO16((uint32) DMA_OUT_Control_PTR));
    // Set the channel transaction descriptor that we just configured.
    CyDmaChSetInitialTd(dmaCh, dmaTd);
    // Finally enable the DMA channel.
    // This will start the first transfer and call the interrupt after every line.
    CyDmaChEnable(dmaCh, 1);

    //
    // Interrup Setup.
    //
    // Set our interrupt for SCANLINE to the interrupt function declared above.
    SCANLINE_StartEx(ScanLine);
    
    //
    // Initialize EEPROM
    //
    // Character set resides here and it is accessible with the following expression:
    //      CY_GET_REG8(CYDEV_EE_BASE + index + (y%8)*256);
    // Where:
    //      index   Is the sprite to be displayed from 0 to 255.
    //      y       Is the current frame buffer line.
    EEPROM_Start();

#if DMA_MEM_CPY
    //
    // DMA memory to memory setup
    //
    // Initialize the DMA channel to transfer from the cframe base address to the dframe base address.
    // This indicates the high 16 bit address that will apply to the low addresses set on the TD.
    // We are going to transfer 64 bytes per burst which is a multiple of the size of the SRAM Spoke data bus (4 bytes).
    damMemCh = DMA_MEM_DmaInitialize(64, 0, HI16((uint32) cframe), HI16((uint32) dframe));
    // current TD index declaration
    int i;
    // Allocate all the TDs first
    // Or we wont be able to chain them.
    for (i=0; i < NUM_MEM_TDS; i++)
    {
        // Alocate a transaction descriptor.
        dmaMemTd[i] = CyDmaTdAllocate();
    }
    // Loop through all the Transaction Descriptors needed to be configured to transfer the full frame buffer.
    // Since we adjusted the full TDs via the preprocessor we can chain all of this and after the loop
    // we can add the final TD
    for (i=0; i < (NUM_MEM_TDS-1); i++)
    {
        // Configure the transaction descriptor for the current transfer.
        // Transfer MEM_TRANSFER_COUNT with auto increment and signalling the end of the last transfer.
        // Set the next TD in the chain.
        CyDmaTdSetConfiguration(dmaMemTd[i], MEM_TRANSFER_COUNT, dmaMemTd[i+1], TD_INC_SRC_ADR | TD_INC_DST_ADR | DMA_MEM__TD_TERMOUT_EN);
        // Set the source and destination addresses for this block of memory transfer.
        CyDmaTdSetAddress(dmaMemTd[i], LO16((uint32)&cframe[(i*MEM_TRANSFER_COUNT)/VGA_X_BYTES][(i*MEM_TRANSFER_COUNT)%VGA_X_BYTES]), LO16((uint32)&dframe[(i*MEM_TRANSFER_COUNT)/VGA_X_BYTES][(i*MEM_TRANSFER_COUNT)%VGA_X_BYTES]));
    }
    //
    // deal with the final TD after the loop.
    //
    // Configure the transaction descriptor for the current transfer.
    // Transfer MEM_TRANSFER_COUNT with auto increment and signalling the end of the last transfer.
    // Set the next TD to disable the transfer after we re done.
#if PARTIAL_TD_COUNT
    CyDmaTdSetConfiguration(dmaMemTd[i], PARTIAL_TD_COUNT, DMA_DISABLE_TD, TD_INC_SRC_ADR | TD_INC_DST_ADR | DMA_MEM__TD_TERMOUT_EN);
#else
    CyDmaTdSetConfiguration(dmaMemTd[i], MEM_TRANSFER_COUNT, DMA_DISABLE_TD, TD_INC_SRC_ADR | TD_INC_DST_ADR | DMA_MEM__TD_TERMOUT_EN);
#endif
    // Set the source and destination addresses for this block of memory transfer.
    CyDmaTdSetAddress(dmaMemTd[i], LO16((uint32)&cframe[(i*MEM_TRANSFER_COUNT)/VGA_X_BYTES][(i*MEM_TRANSFER_COUNT)%VGA_X_BYTES]), LO16((uint32)&dframe[(i*MEM_TRANSFER_COUNT)/VGA_X_BYTES][(i*MEM_TRANSFER_COUNT)%VGA_X_BYTES]));

    // Associate the FrameRdy interrupt code with the FRAME_RDY interrupt.
    FRAME_RDY_StartEx(FrameRdy);
#endif

    CyGlobalIntEnable; /* Enable global interrupts. */

    // Lets just setup something to display in here.
    // for now just setup a border to see if we get it all in frame.
    // lest set it as a define, so we can easily compile out the code
    // when we don't need this test.
    // The first test takes priority if the rest are defined.
#define TEST_CHAR_SET 1
#define TEST_BORDER 1
#if TEST_CHAR_SET
    int x = 0, y = 0;
    for (y = 0; y < VGA_Y_BYTES; y++)
    {
        for (x = 0; x < VGA_X_BYTES; x++)
        {
            // Leave blanks in between characters to place graphical characters separators
            int index = ((y/16)*(VGA_X_BYTES/2)+x/2)%256;
            //
            // On our current mode of 800x600 we have half a line
            // we don't want to use those last 4 pixels.
            // the right way to do this would be to find the modulus of Y bytes by 8 but
            // I'll leave the constant here for this test.
            //
            if (y > (VGA_Y_BYTES-5))
            {
                index = 0x00;
            }
            else if ((y%16)/8 == 0)
            {
                if ((x%2) == 1)
                {
                    // Separator in cross spaces '+'
                    index = 0xc5;
                }
                else
                {
                    // Separator between vertical characters '-'
                    index = 0xc4;
                }
            }
            else if ((x%2) == 1)
            {
                // Separator between horizontal characters '|'
                index = 0xb3;
            }
            // Fill the current frame buffer with the selected character
            // row of pixels.
            cframe[y][x] = CY_GET_REG8(CYDEV_EE_BASE + index + (y%8)*256);
        }
    }
    // Clear the DMA frame buffer, not that it needs it but just in case someone has very fast eyes.
    // and sees the first frame with random pixels.
    memset(dframe, 0, VGA_BUFF_SIZE);
#elif TEST_BORDER
    int x = 0, y = 0;
    for (y = 0; y < VGA_Y_BYTES; y++)
    {
        for (x = 0; x < VGA_X_BYTES; x++)
        {
            // On the first and last line we are setting all the pixels on.
            if ((y == 0) || (y == (VGA_Y_BYTES-1)))
            {
                cframe[y][x] = 0xff;
            }
            else
            {
                // On the rest of the lines.
                if (x == 0)
                {
                    // Set the highest nibble for the left border.
                    cframe[y][x] = 0x80;
                }
                else if (x == (VGA_X_BYTES-1))
                {
                    // Set the lowest nibble for the right border.
                    cframe[y][x] = 0x01;
                }
                else
                {
                    // The rest is all blank (or background color.
                    cframe[y][x] = 0x00;
                }
            }                
        }
    }
#endif

    // We could update the CPU frame buffer (cframe) within the for loop.
    // like for example implement a Pong game.
    // The DMA interrupt and hardware will take care to update the DMA frame buffer.
#if !DMA_MEM_CPY
    volatile int count = 0;
#endif
    // Set initial x and y values for changing the cframe contents
    x = 0, y = 8;
    // Declaration for the current character line counter.
    int n;
    // This is a frame counter that we can use to only process things at a certain frame.
    int frame = 0;
    for(;;)
    {
        // Refresh the screen when the interrupt sets the refresh bit on.
        if (refresh)
        {
            // Disable the per line DMA channel
            CyDmaChDisable(dmaCh);
#if DMA_MEM_CPY
            // Copy the CPU frame buffer into the DMA frame buffer
            // Since this is a software driven DMA we need to trigger each TD
            // but we only  need to set the first transaction descriptor
            // the chain will take care of going for the next one.
            CyDmaChSetInitialTd(damMemCh, dmaMemTd[0]);
            // Enable the channel
            CyDmaChEnable(damMemCh, 1);
            for (i=0; i < NUM_MEM_TDS;i++)
            {
                // Trigger DMA channel using CPU
            	CyDmaChSetRequest(damMemCh, CPU_REQ);
                // Wait for it to be done.
                // FrameRdy Interrupt code will set this to 1 when it's done.
            	while(flag_FrameRdyone == 0);
                // Clear flag for next time.
                flag_FrameRdyone = 0;
            }
            // No need to disable damMemCh since the last TD is set to disable it after completion.
            //CyDmaChDisable(damMemCh);
#else
            //
            // Apparently the memory copy takes longer than the vertical retrace
            // So let's just update 1/10th of the buffer per retrace giving us an update rate of
            // 6fps if the refresh rate is 60Hz
            // We have to make sure VGA_Y_BYTES is divisible by 10 so if we change the resolution
            // we will have to make sure it's still the case.
            memcpy(&dframe[count*VGA_Y_BYTES/10][0], &cframe[count*VGA_Y_BYTES/10][0], VGA_BUFF_SIZE/10);
            if (++count == 10)
            {
                count = 0;
            }
#endif
            frame++;
            // Enable the per line DMA channel
            CyDmaChEnable(dmaCh, 1);
            // We are done refreshing so reset refresh to 0
            refresh = 0;
        }
        else
        {
            // Here we can put code that modifies the CPU frame when we are not busy updating
            // the DMA buffer.

            // For fun lets flip a character of the frame buffer
            // while we are idle at 2fps (every 30 frames), 
            // our current update rate is 6fps (every 10 frames) so its divisable.
            if (frame == 1)
            {
                // Reset the frame counter.
                frame = 0;
                // Flip the current character 8x8 bits
                for (n=0; n<8; n++)
                {
                    cframe[y+n][x] = ~cframe[y+n][x];
                }
                // Update our x and y values for the next loop
                // Only do the characters not the grid so skip every other character.
                x = x+2;
                if (x >= VGA_X_BYTES)
                {
                    // We reached the end of the line so reset the column to 0
                    x = 0;
                    // Increase the row by two characters (16 pixels)
                    y += 16;
                    // Adjust for our last half character since we don't want to get past the buffer
                    // The -4 is specific code for the 100x37.5 (800x600) mode
                    if (y >= VGA_Y_BYTES-4)
                    {
                        // reset to the 2nd line where the characters are.
                        y = 8;
                    }
                }
            }
        }
    }
}
/* [] END OF FILE */

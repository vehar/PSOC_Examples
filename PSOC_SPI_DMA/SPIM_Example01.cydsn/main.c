#include <project.h>
#include <string.h>

#define INLINE_HOT __attribute__((always_inline, hot)) void
/**
 * @file
 * @brief Demonstrates DMA-based transfer of data chunks from a source buffer to a destination
 * buffer.
 *
 * This project uses a DMA controller to transfer data in chunks from a large source buffer
 * to a smaller destination buffer. Each transaction transfers a fixed number of bytes (CHUNK_SIZE),
 * and the DMA transfer is triggered by a control register write. The project ensures that:
 * - The source and destination addresses are correctly aligned.
 * - The transfer size matches the DMA burst limitations (1, 2, or 4 bytes per burst).
 * - Proper incrementing and updating of source and destination addresses for multi-chunk transfers.
 */

// clang-format off
/**
 * @section DMA Notes
 * - **Alignment Requirements**: Source and destination addresses must be aligned to the DMA burst size (1, 2, or 4 bytes).
 * - **Burst Size Limitations**: DMA transfers are limited to 1, 2, or 4 bytes per burst. Larger sizes require multiple bursts.
 * - **Spoke Mechanism**: In DMA, spokes refer to data buses connected to peripherals. Ensure the peripheral is correctly addressed.
 * - **Incrementing Addresses**: To transfer data across contiguous memory regions, enable TD_INC_SRC_ADR (for source) and TD_INC_DST_ADR (for destination).
 * - **Error Handling**: Monitor DMA status flags for error conditions such as overflows or invalid accesses.
 * - **If transfer <=4 bytes per burst, you can avoid DST addr incrementing and src addr update mechanism, just increase TD transferCount, 
 * so src addr will be increased automaticaly during bursts and requests, but dst addr not adr data placed correctly to dst.
 * It works with 1, 2, 4 burst sizes
 */
// clang-format on

/* Definitions */
#define HEADER_SIZE (7u)             /**< Size of the header in the source buffer */
#define CHUNK_SIZE (6u)              /**< Number of bytes transferred per DMA transaction */
#define USEFUL_DATA (CHUNK_SIZE * 4) /**< Total useful data to transfer */
#define TOTAL_SIZE                                                                                 \
    (USEFUL_DATA + HEADER_SIZE + 2) /**< Total source buffer size, including header and padding */

// To force the placement of a const variable in flash, set its type to const

/* Buffers aligned to meet DMA requirements */

/*
https://www.eevblog.com/forum/projects/no-bitbanging-necessary-or-how-to-drive-a-vga-monitor-on-a-psoc-5lp-programmabl/
So let's declare the upper 32KB section.
On the workspace top menu, under "Project" select "Build Settings...". Once that opens navigate to:
PSoc5LPVGA->ARM GCC 4.9-2015-q1-update->Linker->Command Line
and under Custom Flags type: -Wl,--section-start=.ram2=0x20000000
This tells gcc that we have a section named ram2 starting at 0x20000000 (The upper 32KB memory block)
*/
static uint8 bigSource[TOTAL_SIZE] __attribute__((aligned(32), section(".ram2")));
static uint8 smallDest[CHUNK_SIZE * 3] __attribute__((aligned(32), section(".ram2")));

/* DMA channel and transfer descriptor variables */
static uint8 dmaChannel;
static uint8 dmaTd0;

/* Function prototypes */
void DmaSetup(void);
void TriggerHwDmaRequest(void);
void TriggerSwDmaRequest(void);
void FillDebugPattern(void);
void UpdateDmaTdAddress(uint8 td, uint32 srcAddr);

void CopyWithDma(const uint8 *src, uint8 *dest, size_t size);
void CopyWithMemcpy(const uint8 *src, uint8 *dest, size_t size);
void CopyWithLoop(const uint8 *src, uint8 *dest, size_t size);

INLINE_HOT CopyWithDma(const uint8 *src, uint8 *dest, size_t size);
INLINE_HOT UpdateDmaTdDstAddress(uint8 td, uint32 dstAddr);

/**
 * @brief Fills the source buffer with a recognizable debug pattern.
 */
void FillDebugPattern(void)
{
    bigSource[0] = 0xAA;
    bigSource[1] = 0xBB;
    bigSource[2] = 0xCC;
    bigSource[3] = 0xDD;
    bigSource[4] = 0xEE;
    bigSource[5] = 0xFF;
    bigSource[6] = 0xAB;
    for (uint16 i = HEADER_SIZE; i < TOTAL_SIZE; i++)
        bigSource[i] = (uint8)((i - HEADER_SIZE) & 0xFF);
}

/**
 * @brief Configures the DMA channel and transfer descriptor.
 *
 * The DMA is configured to:
 * - Transfer data in 6-byte chunks.
 * - Increment source and destination addresses after each transfer.
 * - Operate in single-request mode.
 */
void DmaSetup(void)
{
    dmaChannel =
        DMA_TX_DmaInitialize(CHUNK_SIZE,             // Bytes per burst
                             1,                      // 1 means each burst need request!
                             HI16(CYDEV_SRAM_BASE),  // Upper 16 bits of source address
                             HI16(CYDEV_SRAM_BASE)); // Upper 16 bits of destination address

    // Or enother variant
    /*
        DMA_TX_DmaInitialize(1, // Bytes per burst
                             0, // 0 means transfer ALL transferCount from TD at a 1 time!
                             HI16(CYDEV_SRAM_BASE),  // Upper 16 bits of source address
                             HI16(CYDEV_SRAM_BASE)); // Upper 16 bits of destination address
    */
    dmaTd0 = CyDmaTdAllocate();

    // Here can be not CHUNK_SIZE but all USEFUL_DATA IF you find out way not to increment DST addr
    // together with src!
    //  Currently this need for correct DMA >4B placing in DST
    // and so only 1 chunk with manual src addr update (chunks increment)
    CyDmaTdSetConfiguration(dmaTd0, CHUNK_SIZE, dmaTd0,
                            TD_INC_SRC_ADR | TD_INC_DST_ADR | DMA_TX__TD_TERMOUT_EN);
    CyDmaTdSetAddress(dmaTd0, LO16((uint32)(bigSource + HEADER_SIZE)), LO16((uint32)(smallDest)));
    CyDmaChSetInitialTd(dmaChannel, dmaTd0);
    CyDmaChEnable(dmaChannel, 1);
}
//#pragma GCC optimize("O3")
#define BIT_BAND_ALIAS_BASE 0x22000000
/* 'byte' should be an address in the SRAM region (0x20000000 to 0x200FFFFF)
   'bit' should be a number 0 to 31 */
#define BIT_BAND_ALIAS_ADDR(byte, bit)                                                             \
    (BIT_BAND_ALIAS_BASE + 32 * ((uint32_t)(byte)-CYREG_SRAM_DATA_MBASE) + 4 * (uint8_t)(bit))

/* 'a' should be an address (uint32_t *) */
#define GET_BIT(a, bit) (*(volatile uint32_t *)BIT_BAND_ALIAS_ADDR((uint32_t)(a), bit))

/* 'val' should be 0 or 1 */
#define SET_BIT(a, bit, val) (GET_BIT(a, bit) = (uint32_t)(val))
#define TEST_BIT(a, bit) (GET_BIT(a, bit) == (uint32)(val))
uint32 foo __attribute__((section(".ram2"))) = 0;

struct bb_Data {
// Bit Band flag bit definitions
int32 SCAL_REQUEST; // Recalibrate shunt a/d request
int32 SHUNTDATA; // Shunt data available flag
int32 SPISETTINGS; // SPI settings available
} bbdata __attribute__((section (".bitband")));

/**
 * @brief Main function that configures the DMA and manages chunked data transfers.
 */
int main(void)
{
    CyGlobalIntEnable;  // Enable global interrupts
    FillDebugPattern(); // Initialize the source buffer with a debug pattern
    DmaSetup();         // Configure the DMA channel and TD

    uint16 currentAddrOffset = HEADER_SIZE; // Offset within the source buffer

    bbdata.SHUNTDATA=0;

    int32 value = bbdata.SHUNTDATA;
    
    bbdata.SHUNTDATA=1;
    
    value = bbdata.SHUNTDATA;
    
    bbdata.SHUNTDATA=4;
    
    value = bbdata.SHUNTDATA;
    
    foo = 0;
    foo |= (1 << 2);
    
    SET_BIT(&foo, 5, 1); /* set bit 5 of foo */
    // if (TEST_BIT(&foo, 5, 1)) { ... } /* test bit 5 */

    //  for (;;)
    //  {
    //      Control_Reg_1_Write(0x80); //400nS between pulses of 15,6nS //BUS CLK == 64MHz
    //  }
    for (;;)
    {
        //__ISB(); // Ensures pipeline is synchronized
        //__DSB(); // Ensures memory is synchronized

        // Limit the offset to avoid accessing out-of-bounds memory
        if (currentAddrOffset >= (HEADER_SIZE + USEFUL_DATA))
            currentAddrOffset = HEADER_SIZE; // Reset to start of useful data

        // CopyWithLoop(bigSource + currentAddrOffset, smallDest, CHUNK_SIZE);

        // CopyWithMemcpy(bigSource + currentAddrOffset, smallDest, CHUNK_SIZE);

        CopyWithDma(bigSource + currentAddrOffset, smallDest, CHUNK_SIZE);

        // Move to the next chunk
        currentAddrOffset += CHUNK_SIZE;

        // Small delay between requests for demonstration purposes
        // CyDelay(100u);
    }
}
//#pragma GCC reset_options

// O3 - speed, Os - size
#pragma GCC optimize("O3")
INLINE_HOT CopyWithDma(const uint8 *src, uint8 *dest, size_t size)
{
    Control_Reg_1_Write(0x40);
    // Update DMA TD for the next chunk
    UpdateDmaTdAddress(dmaTd0, (uint32)(src)); // 1500nS
    // UpdateDmaTdDstAddress(dmaTd0, (uint32)(dest));// Currently not working

    Control_Reg_1_Write(0x40);
    // Trigger a DMA request
    TriggerHwDmaRequest(); // 750nS
    // Copy 6B = 200nS (2*N+6)clocks  from DOC // = 280nS
    // TriggerSwDmaRequest(); // 1700nS
    Control_Reg_1_Write(0x80);
}

INLINE_HOT CopyWithLoop(const uint8 *src, uint8 *dest, size_t size)
{
    Control_Reg_1_Write(0x40);
    for (size_t i = 0; i < size; i++)
        dest[i] = src[i];
    Control_Reg_1_Write(0x80);
}

INLINE_HOT CopyWithMemcpy(const uint8 *src, uint8 *dest, size_t size)
{
    Control_Reg_1_Write(0x40);
    memcpy(dest, src, size);
    Control_Reg_1_Write(0x80);
}

/**
 * @brief Updates the DMA TD source and destination addresses for the next chunk.
 *
 * This function allows for dynamic updates of the DMA source address to support multi-chunk
 * transfers.
 *
 * @param td The transfer descriptor to update.
 * @param srcAddr The new source address.
 */
INLINE_HOT UpdateDmaTdAddress(uint8 td, uint32 srcAddr)
{
     CyDmaTdSetAddress(td, LO16(srcAddr), LO16((uint32)(smallDest))); //1500nS
   // CY_SET_REG16(0x816D, srcAddr); // 500nS - Raw implementation
}

// Need some way not to increment dst addr to avoid manual increment src addr if transferCount not
// only i chunk but all usefull data!
INLINE_HOT UpdateDmaTdDstAddress(uint8 td, uint32 dstAddr)
{
    // TODO
    //  CyDmaTdSetAddress(td, LO16((uint32)(bigSource + HEADER_SIZE)), LO16((uint32)(dstAddr)));
}

/**
 * @brief Triggers a DMA request by writing to a control register.
 */
INLINE_HOT TriggerHwDmaRequest(void)
{
    Control_Reg_1_Write(0x01); // Write to control register to trigger a DMA request
}

INLINE_HOT TriggerSwDmaRequest(void)
{
    CyDmaChSetRequest(dmaChannel, CY_DMA_CPU_REQ); // trigger a DMA request
}
#pragma GCC reset_options

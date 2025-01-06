/*******************************************************************************
* File Name: main.c
*
* Version: 1.10
*
* Description:
*  This is source code for example project of the SPI Master component.
*  Parameters used:
*   Mode                CPHA == 0, CPOL == 0                
*   Data lines          MOSI+MISO
*   Shift direction     MSB First
*   DataBits:           8 
*   Bit Rate            1 Mbps
*   Clock source        External 
*
*  SPI communication test using DMA. 8 bytes are transmitted
*  between SPI Master and SPI Slave.
*  Received data are displayed on LCD. 

*******************************************************************************/

#include <project.h>

void DmaTxConfiguration(void);
void DmaRxConfiguration(void);
void DMATxRestart(void);

/* DMA Configuration for DMA_TX */
#define DMA_TX_BYTES_PER_BURST      (1u)
#define DMA_TX_REQUEST_PER_BURST    (1u)
#define DMA_TX_SRC_BASE             (CYDEV_SRAM_BASE)
#define DMA_TX_DST_BASE             (CYDEV_PERIPH_BASE)

/* DMA Configuration for DMA_RX */
#define DMA_RX_BYTES_PER_BURST      (1u)
#define DMA_RX_REQUEST_PER_BURST    (1u)
#define DMA_RX_SRC_BASE             (CYDEV_PERIPH_BASE)
#define DMA_RX_DST_BASE             (CYDEV_SRAM_BASE)

#define BUFFER_SIZE                 (8u)
#define STORE_TD_CFG_ONCMPLT        (1u)

/* Variable declarations for DMA_TX*/
uint8 txChannel;
uint8 txTD;

/* Variable declarations for DMA_RX */
uint8 rxChannel;
uint8 rxTD;

/* Data buffers */
uint8 txBuffer [BUFFER_SIZE] = {0x0u, 0x01u, 0x03u, 0x07u, 0x11u, 0x33u, 0x77u, 0xFFu};
uint8 rxBuffer[BUFFER_SIZE];

/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
*  Main function performs following functions:
*   1. Starts Character LCD and print project info
*   2. Starts SPI Master component
*   3. Configures the DMA transfer for RX and TX directions
*   4. Displays the results on Character LCD
*******************************************************************************/
int main()
{
    uint8 i;
    
    CyDelay(2000u);
    
    DmaTxConfiguration();
    DmaRxConfiguration();
    
    SPIM_Start();
    
    CyDmaChEnable(rxChannel, STORE_TD_CFG_ONCMPLT);
    CyDmaChEnable(txChannel, STORE_TD_CFG_ONCMPLT);

    for(;;)
    {
        CyDelay(1u);
        DMATxRestart();
    }
}

void DMATxRestart(void)
{
    /*
    CyDmaChDisable(txChannel);
    CyDmaTdSetAddress(txTD,
        LO16((uint32)txBuffer), 
        LO16((uint32)SPIM_TXDATA_PTR)
    );
    CyDmaChSetInitialTd(txChannel, txTD);
*/
    CyDmaChEnable(txChannel, 1);
}

void DmaTxConfiguration()
{
    /* Init DMA, 1 byte bursts, each burst requires a request */ 
    txChannel = DMA_TX_DmaInitialize(DMA_TX_BYTES_PER_BURST, DMA_TX_REQUEST_PER_BURST, 
                                        HI16((uint32)txBuffer), HI16(DMA_TX_DST_BASE));

    txTD = CyDmaTdAllocate();

    /* Configure this Td as follows:
    *  - Increment the source address, but not the destination address   */
    CyDmaTdSetConfiguration(txTD, BUFFER_SIZE, CY_DMA_DISABLE_TD, TD_INC_SRC_ADR);

    /* From the memory to the SPIM */
    CyDmaTdSetAddress(txTD, LO16((uint32)txBuffer), LO16((uint32) SPIM_TXDATA_PTR));
    
    /* Associate the TD with the channel */
    CyDmaChSetInitialTd(txChannel, txTD); 
}    


void DmaRxConfiguration()
{ 
    /* Init DMA, 1 byte bursts, each burst requires a request */ 
    rxChannel = DMA_RX_DmaInitialize(DMA_RX_BYTES_PER_BURST, DMA_RX_REQUEST_PER_BURST,
                                     HI16(DMA_RX_SRC_BASE), HI16((uint32)rxBuffer));

    rxTD = CyDmaTdAllocate();
    
    /* Configure this Td as follows:
    *  - Increment the destination address, but not the source address
    */
    CyDmaTdSetConfiguration(rxTD, BUFFER_SIZE, CY_DMA_DISABLE_TD, TD_INC_DST_ADR);

    /* From the SPIM to the memory */
    CyDmaTdSetAddress(rxTD, LO16((uint32)SPIM_RXDATA_PTR), LO16((uint32)rxBuffer));

    /* Associate the TD with the channel */
    CyDmaChSetInitialTd(rxChannel, rxTD);
}
   
	
/* [] END OF FILE */

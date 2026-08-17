#include "usb_diag.h"

#include <stdio.h>
#include "stm32h7xx_hal.h"

/* PCD handle generated in USB_DEVICE/Target/usbd_conf.c. */
extern PCD_HandleTypeDef hpcd_USB_OTG_HS;


static const char *USB_Diag_DeviceStateName(uint8_t state)
{
    switch (state)
    {
        case USBD_STATE_DEFAULT:
            return "DEFAULT";

        case USBD_STATE_ADDRESSED:
            return "ADDRESSED";

        case USBD_STATE_CONFIGURED:
            return "CONFIGURED";

        case USBD_STATE_SUSPENDED:
            return "SUSPENDED";

        default:
            return "UNKNOWN";
    }
}


static const char *USB_Diag_SpeedName(USBD_SpeedTypeDef speed)
{
    switch (speed)
    {
        case USBD_SPEED_HIGH:
            return "HIGH";

        case USBD_SPEED_FULL:
            return "FULL";

        case USBD_SPEED_LOW:
            return "LOW";

        default:
            return "UNKNOWN";
    }
}


static const char *USB_Diag_EnumSpeedName(uint32_t enum_speed)
{
    switch (enum_speed & 0x3U)
    {
        case 0U:
            return "HS-if-enumerated";

        case 1U:
            return "FS-30/60MHz-PHY-if-enumerated";

        case 2U:
            return "LS-6MHz-PHY-if-enumerated";

        case 3U:
            return "FS-48MHz-PHY-if-enumerated";

        default:
            return "UNKNOWN";
    }
}


void USB_Diag_Dump(const char *reason, const USBD_HandleTypeDef *pdev)
{
    USB_OTG_GlobalTypeDef *usb = USB_OTG_HS;
    USB_OTG_DeviceTypeDef *dev =
        (USB_OTG_DeviceTypeDef *)((uintptr_t)USB_OTG_HS + USB_OTG_DEVICE_BASE);

    uint32_t gotgctl = usb->GOTGCTL;
    uint32_t gahbcfg = usb->GAHBCFG;
    uint32_t gusbcfg = usb->GUSBCFG;
    uint32_t grstctl = usb->GRSTCTL;
    uint32_t gintsts = usb->GINTSTS;
    uint32_t gintmsk = usb->GINTMSK;
    uint32_t gccfg = usb->GCCFG;

    uint32_t dcfg = dev->DCFG;
    uint32_t dctl = dev->DCTL;
    uint32_t dsts = dev->DSTS;

    uint32_t enum_speed = (dsts >> 1U) & 0x3U;
    uint32_t suspend = dsts & 0x1U;
    uint32_t core_host_mode = gintsts & 0x1U;

    if (reason == NULL)
    {
        reason = "snapshot";
    }

    printf("\r\n--- USB HS / ULPI DIAG: %s ---\r\n", reason);

    printf("PCD: state=%lu speed=%u phy=%u dma=%u vbusSense=%u extVbus=%u\r\n",
           (unsigned long)hpcd_USB_OTG_HS.State,
           (unsigned int)hpcd_USB_OTG_HS.Init.speed,
           (unsigned int)hpcd_USB_OTG_HS.Init.phy_itface,
           (unsigned int)hpcd_USB_OTG_HS.Init.dma_enable,
           (unsigned int)hpcd_USB_OTG_HS.Init.vbus_sensing_enable,
           (unsigned int)hpcd_USB_OTG_HS.Init.use_external_vbus);

    printf("CORE: mode=%s GOTGCTL=0x%08lX GAHBCFG=0x%08lX\r\n",
           (core_host_mode != 0U) ? "HOST" : "DEVICE",
           (unsigned long)gotgctl,
           (unsigned long)gahbcfg);

    printf("CORE: GUSBCFG=0x%08lX GRSTCTL=0x%08lX GCCFG=0x%08lX\r\n",
           (unsigned long)gusbcfg,
           (unsigned long)grstctl,
           (unsigned long)gccfg);

    printf("CORE: GINTSTS=0x%08lX GINTMSK=0x%08lX\r\n",
           (unsigned long)gintsts,
           (unsigned long)gintmsk);

    printf("DEV : DCFG=0x%08lX DCTL=0x%08lX DSTS=0x%08lX enumField=%lu(%s) suspend=%lu\r\n",
           (unsigned long)dcfg,
           (unsigned long)dctl,
           (unsigned long)dsts,
           (unsigned long)enum_speed,
           USB_Diag_EnumSpeedName(enum_speed),
           (unsigned long)suspend);

    if (pdev != NULL)
    {
        printf("USBD: state=%s(%u) old=%u speed=%s addr=%u cfg=%lu conn=%u ep0=%lu\r\n",
               USB_Diag_DeviceStateName(pdev->dev_state),
               (unsigned int)pdev->dev_state,
               (unsigned int)pdev->dev_old_state,
               USB_Diag_SpeedName(pdev->dev_speed),
               (unsigned int)pdev->dev_address,
               (unsigned long)pdev->dev_config,
               (unsigned int)pdev->dev_connection_status,
               (unsigned long)pdev->ep0_state);

        printf("REQ : bm=0x%02X bReq=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%u\r\n",
               (unsigned int)pdev->request.bmRequest,
               (unsigned int)pdev->request.bRequest,
               (unsigned int)pdev->request.wValue,
               (unsigned int)pdev->request.wIndex,
               (unsigned int)pdev->request.wLength);
    }

    printf("------------------------------------------\r\n");
}


void USB_Diag_RunStartupWindow(const USBD_HandleTypeDef *pdev,
                               uint32_t duration_ms,
                               uint32_t sample_interval_ms)
{
    uint32_t elapsed = 0U;

    if (sample_interval_ms == 0U)
    {
        sample_interval_ms = 500U;
    }

    USB_Diag_Dump("t=0 after USBD_Start", pdev);

    while (elapsed < duration_ms)
    {
        uint32_t delay_ms = sample_interval_ms;

        if ((duration_ms - elapsed) < delay_ms)
        {
            delay_ms = duration_ms - elapsed;
        }

        HAL_Delay(delay_ms);
        elapsed += delay_ms;

        printf("USB-DIAG sample at %lu ms\r\n", (unsigned long)elapsed);
        USB_Diag_Dump("startup observation", pdev);
    }
}

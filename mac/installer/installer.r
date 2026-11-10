/* installer.r - MoofPortInstall's resources: the alerts and MoofPort's driver as 'MPDV' 128
 * (mp_drvr.r; the installer copies it into the System file as DRVR 47). */
#include "Dialogs.r"
#include "MacTypes.r"

#define MP_DRVR_TYPE 'MPDV'
#define MP_DRVR_ATTRS appheap     /* loaded into the installer's heap, not the system heap */
#include "mp_drvr.r"

/* 128: the result. ^0 is the text. */
resource 'DITL' (128) {
    {
        {150, 230, 170, 300}, Button { enabled, "OK" };
        {10, 20, 140, 300}, StaticText { disabled, "^0" };
    }
};
resource 'ALRT' (128) {
    {40, 40, 220, 360}, 128,
    { OK, visible, sound1, OK, visible, sound1, OK, visible, sound1, OK, visible, sound1 },
    alertPositionMainScreen
};

/* 129: confirm Install. Item 1 = Install, item 2 = Cancel (the default, Return cancels). */
resource 'DITL' (129) {
    {
        {150, 110, 170, 190}, Button { enabled, "Install" };
        {150, 220, 170, 300}, Button { enabled, "Cancel" };
        {10, 20, 140, 300}, StaticText { disabled, "^0" };
    }
};
resource 'ALRT' (129) {
    {40, 40, 220, 360}, 129,
    { Cancel, visible, silent, Cancel, visible, silent, Cancel, visible, silent, Cancel, visible, silent },
    alertPositionMainScreen
};

/* 130: installed already. Item 1 = Update, item 2 = Uninstall, item 3 = Cancel (the default:
 * the alert's "Cancel" bold item is item 2, so items are ordered for that below). */
resource 'DITL' (130) {
    {
        {150, 10, 170, 90}, Button { enabled, "Update" };
        {150, 220, 170, 300}, Button { enabled, "Cancel" };
        {150, 110, 170, 200}, Button { enabled, "Uninstall" };
        {10, 20, 140, 300}, StaticText { disabled, "^0" };
    }
};
resource 'ALRT' (130) {
    {40, 40, 220, 360}, 130,
    { Cancel, visible, silent, Cancel, visible, silent, Cancel, visible, silent, Cancel, visible, silent },
    alertPositionMainScreen
};

resource 'vers' (1) {
    0x01, 0x00, release, 0x00, verUS,
    "1.0",
    "MoofPort 1.0 installer"
};

/* the icon family (mac/icons, scripts/icons.py) */
#define ICONS_INSTALLER
#include "icons.r"

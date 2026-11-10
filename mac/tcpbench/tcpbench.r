/* tcpbench.r - ALRT/DITL 128 for the app. ^0 is the result text. */
#include "Dialogs.r"

resource 'DITL' (128) {
    {
        /* OK button */
        {150, 230, 170, 300}, Button { enabled, "OK" };
        /* result text (filled by ParamText ^0) */
        {10, 20, 140, 300}, StaticText { disabled, "^0" };
    }
};

resource 'ALRT' (128) {
    {40, 40, 220, 360},
    128,
    {
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1
    },
    alertPositionMainScreen
};

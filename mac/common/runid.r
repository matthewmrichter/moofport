/* runid.r - DLOG/DITL 200: the "name this run" dialog (runid.c). */
#include "Dialogs.r"

resource 'DLOG' (200) {
    {60, 60, 180, 420},
    dBoxProc, visible, noGoAway, 0x0, 200, "",
    alertPositionMainScreen
};

resource 'DITL' (200) {
    {
        {85, 270, 105, 340}, Button { enabled, "OK" };
        {85, 180, 105, 250}, Button { enabled, "Cancel" };
        {10, 15, 45, 345}, StaticText { disabled,
            "Name for this run (it goes into the result file's name):" };
        {55, 18, 71, 342}, EditText { enabled, "" };
    }
};

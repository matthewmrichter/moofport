/* test_levers.c: the MP_CTL_TUNE lever presets shared by MoofTune and MoofBatch (mac/common/mp_levers.h),
 * plus a check that both tools' sources use that header and accept every lever name. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t.h"
#include "mp_drv.h"
#include "mp_levers.h"

static MpTune base(void) { MpTune t; memset(&t, 0xAA, sizeof t); mp_tune_defaults(&t); return t; }

static void test_unknown_name_leaves_tune_alone(void)
{
    MpTune t = base(), u = t;
    T_EQ(mp_lever_preset(&t, "new"), 0);
    T_EQ(mp_lever_preset(&t, "keep"), 0);
    T_EQ(mp_lever_preset(&t, ""), 0);
    T_EQ(memcmp(&t, &u, sizeof t), 0);
}

static void test_rx4_chase1(void)
{
    MpTune t = base();
    T_EQ(mp_lever_preset(&t, "rx4"), 1);
    T_EQ(t.flags, MP_TUNE_RX4); T_EQ(t.flags2, 0); T_EQ(t.early, MP_EARLY_OFF); T_EQ(t.idle_ms, 51);
    T_EQ(mp_lever_preset(&t, "chase1"), 1);          /* restarts from the defaults */
    T_EQ(t.flags, 0); T_EQ(t.early, MP_EARLY_1);
}

static void test_steady_family(void)
{
    MpTune t = base();
    T_EQ(mp_lever_preset(&t, "steady"), 1);
    T_EQ(t.burst_ms, 16); T_EQ(t.mid_ms, 16); T_EQ(t.idle_ms, 16); T_EQ(t.step_ms, 0);
    T_EQ(t.flags, MP_TUNE_NOYIELD | MP_TUNE_NOCHASE);
    T_EQ(t.flags2, MP_TUNE2_NOPAUSE | MP_TUNE2_STEADY);
    T_EQ(t.blind_tx, 1);
    T_EQ(mp_lever_preset(&t, "steadyrx4"), 1);
    T_EQ(t.flags, MP_TUNE_NOYIELD | MP_TUNE_NOCHASE | MP_TUNE_RX4);
    T_EQ(t.flags2, MP_TUNE2_NOPAUSE | MP_TUNE2_STEADY);
    T_EQ(mp_lever_preset(&t, "steadyack"), 1);
    T_EQ(t.burst_ms, 16);
    T_EQ(t.flags, MP_TUNE_NOYIELD | MP_TUNE_NOCHASE | MP_TUNE_ACK2);
    T_EQ(t.flags2, MP_TUNE2_NOPAUSE);               /* STEADY off: the driver keeps its post-WRITE poll */
    T_EQ(t.early, MP_EARLY_OFF);
}

/* Both tools must use the shared header, and MoofBatch must name every setting this task adds. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

static void test_tools_share_the_table(void)
{
    char *tune = slurp(MP_SRC_DIR "/mac/mooftune/mooftune.c");
    char *batch = slurp(MP_SRC_DIR "/mac/tcpsend/tcpsend.c");
    T_CHECK(tune != NULL && batch != NULL);
    if (tune && batch) {
        T_CHECK(strstr(tune, "#include \"mp_levers.h\"") != NULL);
        T_CHECK(strstr(batch, "#include \"mp_levers.h\"") != NULL);
        T_CHECK(strstr(tune, "mp_lever_preset(t, id)") != NULL);
        T_CHECK(strstr(batch, "mp_lever_preset(&t, name)") != NULL);
        T_CHECK(strstr(batch, "strcmp(name, \"keep\")") != NULL);
    }
    free(tune); free(batch);
}

T_MAIN(test_unknown_name_leaves_tune_alone, test_rx4_chase1, test_steady_family, test_tools_share_the_table)

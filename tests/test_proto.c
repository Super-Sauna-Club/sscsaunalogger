/* Host-Test fuer ssc_proto.h: CRC, Meta-Siegel, ID-Validierung, Laengen. */
#include "../SenseCAP_Indicator_RP2040/src/ssc/ssc_proto.h"
#include <stdio.h>
#include <string.h>
static int fail = 0;
#define CHECK(c, m) do { if (!(c)) { fail++; printf("  FAIL: %s\n", m); } } while (0)
int main(void) {
    /* CRC16-CCITT-FALSE Referenz: "123456789" -> 0x29B1 */
    CHECK(ssc_crc16("123456789", 9) == 0x29B1, "crc16 referenzwert");
    ssc_meta_wire_t m; memset(&m, 0, sizeof m);
    strcpy(m.id, "S20261029_183000"); strcpy(m.operator_tag, "Bernhard");
    CHECK(ssc_meta_check(&m) == 0, "legacy meta ohne siegel");
    ssc_meta_seal(&m);
    CHECK(ssc_meta_check(&m) == 1, "meta siegel gueltig");
    m.notes[5] ^= 1;
    CHECK(ssc_meta_check(&m) == -1, "meta korruption erkannt");
    CHECK(ssc_id_valid("S20261029_183000", SSC_ID_LEN), "id gueltig");
    CHECK(!ssc_id_valid("", SSC_ID_LEN), "leere id");
    CHECK(!ssc_id_valid("../x", SSC_ID_LEN), "pfad-id");
    CHECK(!ssc_id_valid("S20261029_183000_ABCDEFGH", SSC_ID_LEN), "zu lange id");
    CHECK(ssc_cmd_len(SSC_CMD_SESSION_START) == 1, "start len");
    CHECK(ssc_cmd_len(SSC_CMD_META_PUSH) == 302, "meta len");
    CHECK(ssc_cmd_len(0x42) == -1, "unbekannt");
    printf(fail ? "%d FEHLER\n" : "PROTO OK\n", fail);
    return fail != 0;
}

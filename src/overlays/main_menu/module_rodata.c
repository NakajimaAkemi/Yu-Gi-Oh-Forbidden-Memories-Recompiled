#include "../../types.h"
#include "card_comparators.h"
#include "module_rodata.h"
/* The entries are four bytes each, which a 64-bit build cannot spend on a
 * native address: see guest_fn.h, and the addresses below are the retail
 * ones from config/slus_01411/overlays/main_menu_symbols.txt. */
#include "pc/compat/guest_fn.h"

typedef s32 (*MainMenuComparator)();

const MainMenuComparators D_80180004 = {
    {
        GUEST_FN(MainMenuComparator, MainMenu_CompareCardsByName, 0x8018416C),
        GUEST_FN(MainMenuComparator, MainMenu_CompareCardsByMaxStat, 0x80183514),
        GUEST_FN(MainMenuComparator, MainMenu_CompareCardsByAttack, 0x801836F4),
        GUEST_FN(MainMenuComparator, MainMenu_CompareCardsByDefense, 0x80183884),
        GUEST_FN(MainMenuComparator, MainMenu_CompareCardsByType, 0x80183A14),
        GUEST_FN(MainMenuComparator, MainMenu_CompareCardsByCount, 0x80184254),
    }
};

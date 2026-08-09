#ifndef FUNPLUGIN_RESOURCE_H
#define FUNPLUGIN_RESOURCE_H

/* Dialog */
#define IDD_SETTINGS        100

/* Move settings */
#define IDC_MOVE_INTERVAL   1001  /* edit: seconds between moves          */
#define IDC_NE_ASK          1002  /* radio: ask when no empty channel     */
#define IDC_NE_ALL          1003  /* radio: use all channels              */
#define IDC_NE_SKIP         1004  /* radio: skip until one frees up        */
#define IDC_AUTOCREATE      1005  /* checkbox: auto-create temp channels   */

/* Poke settings */
#define IDC_POKE_DELAY      1010  /* edit: seconds between pokes           */
#define IDC_PM_CUSTOM       1011  /* radio: custom message                 */
#define IDC_PM_RANDOM       1012  /* radio: random message from list       */
#define IDC_CUSTOMMSG       1013  /* edit: the custom poke message         */
#define IDC_RANDOMMSGS      1014  /* multiline edit: one message per line   */
#define IDC_PM_RANDCHARS    1015  /* radio: random letters/numbers          */
#define IDC_RANDLEN         1016  /* edit: length of random letters/numbers */

/* Global */
#define IDC_LIMITER         1020  /* edit: auto-stop after N seconds (0=off) */

/* Reactions */
#define IDC_AUTOPOKEBACK    1030  /* checkbox: poke back when poked          */
#define IDC_APB_TIMES       1031  /* edit: pokes per auto-poke-back          */
#define IDC_TALK_TIMES      1032  /* edit: pokes per talk trigger            */

/* Static labels (no logic, just layout) */
#define IDC_LBL_MOVE        1100
#define IDC_LBL_POKE        1101
#define IDC_LBL_GLOBAL      1102
#define IDC_LBL_REACT       1103

#endif /* FUNPLUGIN_RESOURCE_H */

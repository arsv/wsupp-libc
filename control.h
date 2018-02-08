#define TAGGED(a, b, c) ((a << 24) | (b << 16) | (c))

#define WICTL "/run/ctrl/wsupp"
#define WICFG "/var/wipsk"
#define WICAP "/var/wiap"
#define RESOLV_CONF "/run/resolv.conf"

#define WI(c) TAGGED('W', 'I', c)

#define CMD_WI_STATUS       WI(0)
#define CMD_WI_DEVICE       WI(1)
#define CMD_WI_SCAN         WI(2)
#define CMD_WI_NEUTRAL      WI(3)
#define CMD_WI_CONNECT      WI(4)
#define CMD_WI_FORGET       WI(5)

#define REP_WI_NET_DOWN     WI(0)
#define REP_WI_SCANNING     WI(1)
#define REP_WI_SCAN_DONE    WI(2)
#define REP_WI_SCAN_FAIL    WI(3)
#define REP_WI_DISCONNECT   WI(4)
#define REP_WI_NO_CONNECT   WI(5)
#define REP_WI_CONNECTED    WI(6)

#define ATTR_SSID       1
#define ATTR_PSK        2
#define ATTR_PRIO       3
#define ATTR_SIGNAL     4
#define ATTR_FREQ       5
#define ATTR_TYPE       6
#define ATTR_BSSID      7
#define ATTR_SCAN       8
#define ATTR_IFI        9
#define ATTR_NAME      10
#define ATTR_STATE     11
#define ATTR_IP        12
#define ATTR_MASK      13
#define ATTR_LINK      14
#define ATTR_MODE      15
#define ATTR_FLAGS     16
#define ATTR_ADDR      17

#define WS_IDLE         0
#define WS_RFKILLED     1
#define WS_NETDOWN      2
#define WS_EXTERNAL     3
#define WS_SCANNING     4
#define WS_CONNECTING   5
#define WS_CONNECTED    6

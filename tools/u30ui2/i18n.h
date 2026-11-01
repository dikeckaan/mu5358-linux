/*
 * i18n: every string the screen shows, in English, Turkish and Simplified Chinese. T(S_X) gives the current
 * language's; the language comes from /etc/mu5358/lang (en|tr|zh) and the settings page.
 * mkfont.sh reads this file: every non-ASCII character here is in the CJK font subset the binary carries.
 * SPDX-License-Identifier: MIT
 */
#ifndef I18N_H
#define I18N_H

enum { L_EN, L_TR, L_ZH, NLANG };

#define STRINGS(X) \
	X(S_LANG_NAME, "English", "Türkçe", "中文") \
	X(S_NETWORK, "Network", "Ağ", "网络") \
	X(S_WIFI, "Wi-Fi", "Wi-Fi", "Wi-Fi") \
	X(S_CLIENTS, "Clients", "İstemciler", "客户端") \
	X(S_TRAFFIC, "Traffic", "Trafik", "流量") \
	X(S_MESSAGES, "Messages", "Mesajlar", "短信") \
	X(S_BATTERY, "Battery", "Pil", "电池") \
	X(S_SYSTEM, "System", "Sistem", "系统") \
	X(S_SETTINGS, "Settings", "Ayarlar", "设置") \
	X(S_POWER, "Power", "Güç", "电源") \
	X(S_LOG, "Log", "Günlük", "日志") \
	X(S_NO_SIM, "No SIM", "SIM yok", "无 SIM 卡") \
	X(S_SEARCHING, "Searching…", "Aranıyor…", "搜索中…") \
	X(S_NO_SERVICE, "No service", "Servis yok", "无服务") \
	X(S_ROAMING, "Roaming", "Dolaşım", "漫游") \
	X(S_CONNECTED, "Connected", "Bağlı", "已连接") \
	X(S_DISCONNECTED, "Not connected", "Bağlı değil", "未连接") \
	X(S_OPERATOR, "Operator", "Operatör", "运营商") \
	X(S_TYPE, "Type", "Tür", "类型") \
	X(S_BAND, "Band", "Bant", "频段") \
	X(S_CELL, "Cell", "Hücre", "小区") \
	X(S_SIGNAL, "Signal", "Sinyal", "信号") \
	X(S_IP, "IP", "IP", "IP") \
	X(S_APN, "APN", "APN", "APN") \
	X(S_TODAY, "Today", "Bugün", "今天") \
	X(S_TODAY_LC, "today", "bugün", "今天") \
	X(S_THIS_MONTH, "This month", "Bu ay", "本月") \
	X(S_LAST_60S, "last 60 s", "son 60 sn", "最近 60 秒") \
	X(S_SIGNAL_10MIN, "last 10 min", "son 10 dk", "最近 10 分钟") \
	X(S_DOWNLOAD, "Download", "İndirme", "下载") \
	X(S_UPLOAD, "Upload", "Yükleme", "上传") \
	X(S_TOTAL, "Total", "Toplam", "总计") \
	X(S_SINCE_BOOT, "since boot", "açılıştan beri", "开机以来") \
	X(S_ONE_CLIENT, "1 client", "1 istemci", "1 个客户端") \
	X(S_N_CLIENTS, "%d clients", "%d istemci", "%d 个客户端") \
	X(S_NO_CLIENTS, "No clients", "İstemci yok", "暂无客户端") \
	X(S_HOTSPOT, "Hotspot", "Hotspot", "热点") \
	X(S_PASSWORD, "Password", "Parola", "密码") \
	X(S_CHANNEL, "Channel", "Kanal", "信道") \
	X(S_SHOW_QR, "Show QR code", "QR kodu göster", "显示二维码") \
	X(S_SCAN_TO_JOIN, "Scan to join", "Okutup bağlan", "扫码连接") \
	X(S_TAP_FOR_DETAILS, "Tap for details", "Ayrıntı için dokun", "点按查看详情") \
	X(S_ON, "On", "Açık", "开") \
	X(S_OFF, "Off", "Kapalı", "关") \
	X(S_NO_MESSAGES, "No messages", "Mesaj yok", "没有短信") \
	X(S_SWIPE_DOWN_CLOSE, "Swipe down to close", "Kapatmak için aşağı kaydır", "下滑关闭") \
	X(S_CHARGING, "Charging", "Şarj oluyor", "充电中") \
	X(S_ON_BATTERY, "On battery", "Pilde", "使用电池") \
	X(S_FULL, "Full", "Dolu", "已充满") \
	X(S_USB_POWER, "USB power", "USB güç", "USB 供电") \
	X(S_LEFT, "left", "kaldı", "剩余") \
	X(S_TO_FULL, "to full", "dolmasına", "充满还需") \
	X(S_TEMPERATURE, "Temperature", "Sıcaklık", "温度") \
	X(S_VOLTAGE, "Voltage", "Gerilim", "电压") \
	X(S_CURRENT, "Current", "Akım", "电流") \
	X(S_HEALTH, "Health", "Sağlık", "健康") \
	X(S_CPU, "CPU", "İşlemci", "处理器") \
	X(S_MEMORY, "Memory", "Bellek", "内存") \
	X(S_STORAGE, "Storage", "Depolama", "存储") \
	X(S_LOAD, "Load", "Yük", "负载") \
	X(S_UPTIME, "Uptime", "Çalışma", "运行时间") \
	X(S_TEMPS, "Temperatures", "Sıcaklıklar", "温度") \
	X(S_SOC, "SoC", "SoC", "芯片") \
	X(S_WIFI_CHIP, "Wi-Fi chip", "Wi-Fi çipi", "Wi-Fi 芯片") \
	X(S_PA, "RF amp", "RF yükselteç", "射频功放") \
	X(S_KERNEL, "Kernel", "Çekirdek", "内核") \
	X(S_VERSION, "Version", "Sürüm", "版本") \
	X(S_LANGUAGE, "Language", "Dil", "语言") \
	X(S_SCREEN_OFF, "Screen off", "Ekran kapanma", "息屏") \
	X(S_NEVER, "Never", "Hiç", "从不") \
	X(S_SECONDS, "%d s", "%d sn", "%d 秒") \
	X(S_MINUTES, "%d min", "%d dk", "%d 分钟") \
	X(S_HOURS_MIN, "%dh %02dm", "%ds %02ddk", "%d小时%02d分") \
	X(S_DAYS_H, "%dd %dh", "%dg %ds", "%d天%d小时") \
	X(S_LOCK, "Lock", "Kilit", "锁定") \
	X(S_HOLD, "Hold to unlock", "Basılı tut", "长按解锁") \
	X(S_MOBILE_DATA, "Mobile data", "Mobil veri", "移动数据") \
	X(S_INTERFACE, "Interface", "Arayüz", "界面") \
	X(S_MODERN, "Dashboard", "Dashboard", "仪表盘") \
	X(S_CLASSIC, "Classic", "Klasik", "经典") \
	X(S_BRIGHTNESS, "Brightness", "Parlaklık", "亮度") \
	X(S_USB_DRIVE, "USB drive", "USB bellek", "U 盘") \
	X(S_RESTART, "Restart → Android", "Yeniden başlat → Android", "重启 → Android") \
	X(S_POWER_OFF, "Power off", "Kapat", "关机") \
	X(S_RESTART_MODEM, "Restart modem", "Modemi yeniden başlat", "重启调制解调器") \
	X(S_RESTART_WIFI, "Restart Wi-Fi", "Wi-Fi'yi yeniden başlat", "重启 Wi-Fi") \
	X(S_SCREEN_OFF_NOW, "Screen off", "Ekranı kapat", "关闭屏幕") \
	X(S_HOLD_1S, "Hold 1 s", "1 sn basılı tut", "长按 1 秒") \
	X(S_LOG_EMPTY, "Log is empty", "Günlük boş", "日志为空") \
	X(S_LINUX, "Linux", "Linux", "Linux") \
	X(S_MODEM, "Modem", "Modem", "调制解调器") \
	X(S_DATA_OFF, "Mobile data off", "Mobil veri kapalı", "移动数据已关闭") \
	X(S_DAY, "day", "gün", "天") \
	X(S_DAYS, "days", "gün", "天") \
	X(S_UNLOCK_HINT, "Hold to unlock", "Açmak için basılı tut", "长按解锁")

enum {
#define X(id, en, tr, zh) id,
	STRINGS(X)
#undef X
	NSTRINGS
};

extern int lang;
const char *T(int id);
void lang_load(void);
void lang_save(void);
#endif

/*
 * Copyright 2026, Techniix. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _IF_MT7921VAR_H_
#define _IF_MT7921VAR_H_

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/taskqueue.h>
#include <sys/callout.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>


enum mt7921_chip {
	MT7921_CHIP_7921,
	MT7921_CHIP_7922,
	MT7921_CHIP_7920,
	MT7921_CHIP_7902,
};

struct mt7921_ident {
	uint16_t		vendor;
	uint16_t		device;
	enum mt7921_chip	chip;
	const char*		name;
};

/*
 * How far attach goes. Set in
 *   /boot/home/config/settings/kernel/drivers/mediatekwifi7921
 * with a line such as "stage 2". Default is MT7921_STAGE_MAX.
 */
enum {
	MT7921_STAGE_CHIP = 1,		/* milestone 1: chip ID, reset, firmware files */
	MT7921_STAGE_DMA = 2,		/* + set up and enable DMA rings, then stop */
	MT7921_STAGE_FIRMWARE = 3,	/* + boot firmware, read MAC address */
	MT7921_STAGE_NET80211 = 4,	/* + interrupts, register with net80211, stay */
	MT7921_STAGE_SCAN = 5,		/* + MAC/radio setup, firmware scanning */
	MT7921_STAGE_CONNECT = 6,	/* + transmit, join networks (open/WPA2) */
	MT7921_STAGE_MAX = MT7921_STAGE_CONNECT
};

/*
 * Channel lists, shared by net80211 (mt7921_net.c) and the firmware
 * channel domain (mt7921_mcu.c). 5 GHz 52-144 are DFS: listen only, the
 * station never transmits there before it hears an access point.
 */
#define MT7921_CHANS_2GHZ	{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }
#define MT7921_CHANS_5GHZ	{ 36, 40, 44, 48, 52, 56, 60, 64, \
	100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, \
	149, 153, 157, 161, 165 }
#define MT7921_CHAN_IS_DFS(c)	((c) >= 52 && (c) <= 144)

#define MT7921_TX_SLOTS		64	/* data TX ring size; also token space */

/* Hardware DMA descriptor, 16 bytes, little endian (mt76/dma.h). */
struct mt7921_desc {
	uint32_t	buf0;
	uint32_t	ctrl;
	uint32_t	buf1;
	uint32_t	info;
} __packed;

struct mt7921_ring {
	const char*		name;
	uint32_t		regs;		/* chip address of the ring's register block */
	int			hw_idx;
	int			ndesc;
	int			bufsize;	/* 0: ring has no buffers */
	bool			is_rx;
	uint8_t			busy[MT7921_TX_SLOTS];	/* data TX: token in flight */

	int			head;		/* next slot the driver fills */
	int			tail;		/* next slot the driver consumes (RX) */

	bus_dma_tag_t		desc_tag;
	bus_dmamap_t		desc_map;
	volatile struct mt7921_desc* desc;
	bus_addr_t		desc_paddr;

	bus_dma_tag_t		buf_tag;
	bus_dmamap_t		buf_map;
	uint8_t*		buf;
	bus_addr_t		buf_paddr;
};

/* WTBL entry used for the BSS (MT792x_WTBL_RESERVED - bss_idx 0). */
#define MT7921_WCID_BSS		19
/* WTBL entry for the access point we join (first station entry). */
#define MT7921_WCID_STA		1


/* What the firmware needs to know about the AP (station record). */
struct mt7921_sta_info {
	uint8_t		addr[6];
	uint16_t	aid;
	bool		is_5ghz;
	uint16_t	basic_rates;	/* bitmap over the band's rate table */
	uint16_t	supp_rates;
	uint8_t		state;		/* MT7921_STA_STATE_* */
	bool		enable;
	bool		newly;
};
#define MT7921_STA_STATE_NONE	0
#define MT7921_STA_STATE_AUTH	1
#define MT7921_STA_STATE_ASSOC	2

/* What the firmware needs to know about the BSS. */
struct mt7921_bss_info {
	uint8_t		bssid[6];
	uint8_t		channel;
	bool		is_5ghz;
	uint16_t	beacon_int;
	uint8_t		dtim_period;
	bool		enable;
};

struct mt7921_scan_target {
	uint8_t		len;
	uint8_t		ssid[32];
};

struct mt7921_vap {
	struct ieee80211vap	mv_vap;
	int			(*mv_newstate)(struct ieee80211vap*,
				    enum ieee80211_state, int);
};
#define MT7921_VAP(vap)		((struct mt7921_vap*)(vap))

struct mt7921_softc {
	struct ieee80211com	sc_ic;		/* must stay usable by net80211 */
	device_t		sc_dev;
	struct mtx		sc_mtx;

	struct resource*	sc_mem;
	int			sc_mem_rid;
	struct resource*	sc_irq;
	int			sc_irq_rid;
	void*			sc_ih;

	enum mt7921_chip	sc_chip;
	uint32_t		sc_rev;		/* (chipid << 16) | rev */
	int			sc_stage;

	const char*		sc_fw_patch;	/* firmware(9) names */
	const char*		sc_fw_ram;

	/* DMA rings (milestone 2). */
	bool			sc_dma_up;
	bool			sc_dma_paused;	/* engine off + no bus mastering */
	struct callout		sc_shutdown_co;	/* watches gKernelShutdown */
	bool			sc_shutdown_armed;
	bool			sc_shutting_down;
	struct mt7921_ring	sc_tx_data0;
	struct mt7921_ring	sc_tx_fwdl;
	struct mt7921_ring	sc_tx_wm;
	struct mt7921_ring	sc_rx_mcu;	/* events before firmware runs */
	struct mt7921_ring	sc_rx_wa;	/* events after firmware runs */
	struct mt7921_ring	sc_rx_data;

	/* MCU (milestone 3). */
	struct mt7921_mcu*	sc_mcu;
	uint8_t			sc_mcu_seq;
	uint8_t			sc_macaddr[6];
	char			sc_fw_version[32];

	/* Interrupts (milestone 4). */
	volatile bool		sc_intr_ready;	/* read by the hard IRQ glue */
	uint32_t		sc_irqmask;
	int			sc_intr_logged;
	int			sc_mcu_evt_logged;

	/* net80211 (milestone 4). */
	bool			sc_ic_attached;
	int			sc_state_logged;
	bool			sc_parent_logged;

	/* Receive path and scanning (milestone 5). */
	bool			sc_radio_ready;	/* MAC/radio set up in firmware */
	struct mbufq		sc_rxq;		/* frames waiting for net80211 */
	struct task		sc_rx_task;
	struct task		sc_scan_done_task;
	struct callout		sc_scan_to;
	bool			sc_scanning;
	uint8_t			sc_scan_seq;
	int			sc_rx_logged;
	uint32_t		sc_rx_frames;
	uint32_t		sc_rx_dropped;

	/* Transmit and joining (milestone 6). */
	bool			sc_can_tx;	/* stage >= 6 */
	bool			sc_sta_added;	/* AP station record in firmware */
	uint8_t			sc_bssid[6];
	bool			sc_bss_5ghz;
	uint8_t			sc_bss_channel;
	volatile bool		sc_roc_granted;
	bool			sc_roc_active;
	uint8_t			sc_roc_token;
	int			sc_tx_logged;
	int			sc_txfree_logged;
	uint32_t		sc_tx_frames;
	uint32_t		sc_tx_failed;
};

#define MT7921_LOCK_INIT(sc) \
	mtx_init(&(sc)->sc_mtx, device_get_nameunit((sc)->sc_dev), \
	    MTX_NETWORK_LOCK, MTX_DEF)
#define MT7921_LOCK(sc)		mtx_lock(&(sc)->sc_mtx)
#define MT7921_UNLOCK(sc)	mtx_unlock(&(sc)->sc_mtx)
#define MT7921_LOCK_DESTROY(sc)	mtx_destroy(&(sc)->sc_mtx)

/* if_mt7921.c: register access by chip address (goes through the remap). */
uint32_t	mt7921_rr(struct mt7921_softc*, uint32_t addr);
void		mt7921_wr(struct mt7921_softc*, uint32_t addr, uint32_t val);
uint32_t	mt7921_rmw(struct mt7921_softc*, uint32_t addr, uint32_t mask,
		    uint32_t val);
uint32_t	mt7921_l1_rr(struct mt7921_softc*, uint32_t addr);
uint32_t	mt7921_l1_map(struct mt7921_softc*, uint32_t addr);
bool		mt7921_wtbl_update(struct mt7921_softc*, int idx);
bool		mt7921_poll_msec(struct mt7921_softc*, uint32_t addr,
		    uint32_t mask, uint32_t val, int timeout_ms, int tick_ms);

#define mt7921_set(sc, a, v)	mt7921_rmw(sc, a, 0, v)
#define mt7921_clear(sc, a, v)	mt7921_rmw(sc, a, v, 0)

/* mt7921_dma.c */
int		mt7921_dma_init(struct mt7921_softc*);
void		mt7921_dma_scrub(struct mt7921_softc*);
void		mt7921_dma_stop(struct mt7921_softc*);
void		mt7921_dma_free_rings(struct mt7921_softc*);
int		mt7921_dma_tx(struct mt7921_softc*, struct mt7921_ring*,
		    const void* data, int len);
int		mt7921_dma_tx_get_slot(struct mt7921_softc*, struct mt7921_ring*,
		    uint8_t** buf, bus_addr_t* paddr);
void		mt7921_dma_tx_post(struct mt7921_softc*, struct mt7921_ring*,
		    int idx, int len);
void		mt7921_dma_tx_token_free(struct mt7921_softc*, struct mt7921_ring*,
		    int token);
int		mt7921_dma_rx_poll(struct mt7921_softc*, struct mt7921_ring*,
		    int (*handler)(struct mt7921_softc*, const uint8_t*, int, void*),
		    void* arg);

/* mt7921_mcu.c */
struct mt7921_mcu;
int		mt7921_mcu_alloc(struct mt7921_softc*);
void		mt7921_mcu_free(struct mt7921_softc*);
int		mt7921_mcu_run_firmware(struct mt7921_softc*);
int		mt7921_mcu_init_mac(struct mt7921_softc*);
int		mt7921_mcu_bss_basic(struct mt7921_softc*, const uint8_t* bssid,
		    bool active);
int		mt7921_mcu_hw_scan(struct mt7921_softc*, bool active,
		    const struct mt7921_scan_target* ssids, int n_ssids,
		    const uint8_t (*chans)[2], int n_chans);
int		mt7921_mcu_cancel_hw_scan(struct mt7921_softc*);
int		mt7921_mcu_sta_update(struct mt7921_softc*,
		    const struct mt7921_sta_info*);
int		mt7921_mcu_bss_update(struct mt7921_softc*,
		    const struct mt7921_bss_info*);
int		mt7921_mcu_set_roc(struct mt7921_softc*, uint8_t channel,
		    bool is_5ghz, uint32_t duration_ms);
int		mt7921_mcu_abort_roc(struct mt7921_softc*);
void		mt7921_mcu_poll_locked(struct mt7921_softc*);

/* mt7921_net.c */
int		mt7921_net_attach(struct mt7921_softc*);
void		mt7921_net_detach(struct mt7921_softc*);
void		mt7921_rx_frame(struct mt7921_softc*, const uint8_t* data, int len);
void		mt7921_scan_done_event(struct mt7921_softc*);
void		mt7921_tx_free_event(struct mt7921_softc*, const uint8_t* data,
		    int len);
void		mt7921_roc_grant_event(struct mt7921_softc*);

/* if_mt7921.c */
void		mt7921_intr(void*);
void		mt7921_hw_pause(struct mt7921_softc*);
void		mt7921_hw_resume(struct mt7921_softc*);

/* glue.c */
int		mt7921_haiku_get_stage(void);
bool		mt7921_haiku_shutting_down(void);

#endif /* _IF_MT7921VAR_H_ */

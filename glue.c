/*
 * Copyright 2026, Techniix. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <sys/bus.h>
#include <sys/kernel.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

#include <driver_settings.h>

#include <dev/mt7921/if_mt7921var.h>
#include <dev/mt7921/mt7921_reg.h>


HAIKU_FBSD_WLAN_DRIVER_GLUE(mediatekwifi7921, mt7921, pci)
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(FBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
/*
 * Files are looked up in
 *   /boot/system/non-packaged/data/firmware/mediatekwifi7921/  (checked first)
 *   /boot/system/data/firmware/mediatekwifi7921/
 * Copy them from linux-firmware's mediatek/ directory.
 */
HAIKU_FIRMWARE_NAME_MAP({
	{"mt7921_patch", "WIFI_MT7961_patch_mcu_1_2_hdr.bin"},
	{"mt7921_ram", "WIFI_RAM_CODE_MT7961_1.bin"},
	{"mt7922_patch", "WIFI_MT7922_patch_mcu_1_1_hdr.bin"},
	{"mt7922_ram", "WIFI_RAM_CODE_MT7922_1.bin"},
});


/*
 * Milestone 1 doesn't call net80211 yet, so nothing pulls the WLAN glue
 * (init_wlan_stack, start_wlan, ...) out of freebsd_wlan.a and the link
 * fails. Reference it explicitly. Harmless once the driver uses net80211.
 */
static void* const __mt7921_force_wlan_link __attribute__((used))
	= (void*)&init_wlan_stack;


/*
 * Hard-interrupt half. Port of mt792x_irq_handler(): mask all host
 * interrupts and let the interrupt thread (mt7921_intr) do the work; it
 * unmasks them again when done. The vector is MSI, so it is always ours
 * once the driver has set it up.
 */
int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	struct mt7921_softc* sc = (struct mt7921_softc*)device_get_softc(dev);

	if (sc == NULL || !sc->sc_intr_ready)
		return 0;

	mt7921_wr(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	return 1;
}


/*
 * Bring-up safety switch. Create the text file
 *   /boot/home/config/settings/kernel/drivers/mediatekwifi7921
 * containing for example
 *   stage 1
 * to stop after milestone 1 (no DMA), or "stage 2" to stop after setting
 * up DMA (no firmware). Without the file, every stage runs.
 */
int
mt7921_haiku_get_stage(void)
{
	int stage = MT7921_STAGE_MAX;
	void* handle = load_driver_settings("mediatekwifi7921");

	if (handle != NULL) {
		const char* value = get_driver_parameter(handle, "stage", NULL, NULL);
		if (value != NULL && value[0] >= '1' && value[0] <= '9'
			&& value[1] == '\0') {
			int v = value[0] - '0';
			if (v <= MT7921_STAGE_MAX)
				stage = v;
		}
		unload_driver_settings(handle);
	}
	return stage;
}


/* "ht off" in the same settings file turns 802.11n off (legacy rates). */
bool
mt7921_haiku_ht_enabled(void)
{
	bool enabled = true;
	void* handle = load_driver_settings("mediatekwifi7921");

	if (handle != NULL) {
		enabled = get_driver_boolean_parameter(handle, "ht", true, true);
		unload_driver_settings(handle);
	}
	return enabled;
}


/*
 * Set by the kernel's system_shutdown() before teams are ended and disks
 * synced (headers/private/kernel/kernel.h). There is no driver shutdown
 * hook, so the driver polls this to quiesce the chip before a reboot.
 */
extern bool gKernelShutdown;

bool
mt7921_haiku_shutting_down(void)
{
	return gKernelShutdown;
}

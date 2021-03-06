/*-
 * Copyright (c) 2016 Jared McNeill <jmcneill@invisible.ca>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

/*
 * Allwinner thermal sensor controller
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/reboot.h>
#include <sys/module.h>
#include <sys/cpu.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>

#include <arm/allwinner/aw_sid.h>

#include "cpufreq_if.h"

#define	THS_CTRL0		0x00
#define	THS_CTRL1		0x04
#define	 ADC_CALI_EN		(1 << 17)
#define	THS_CTRL2		0x40
#define	 SENSOR_ACQ1_SHIFT	16
#define	 SENSOR2_EN		(1 << 2)
#define	 SENSOR1_EN		(1 << 1)
#define	 SENSOR0_EN		(1 << 0)
#define	THS_INTC		0x44
#define	THS_INTS		0x48
#define	 THS2_DATA_IRQ_STS	(1 << 10)
#define	 THS1_DATA_IRQ_STS	(1 << 9)
#define	 THS0_DATA_IRQ_STS	(1 << 8)
#define	 SHUT_INT2_STS		(1 << 6)
#define	 SHUT_INT1_STS		(1 << 5)
#define	 SHUT_INT0_STS		(1 << 4)
#define	 ALARM_INT2_STS		(1 << 2)
#define	 ALARM_INT1_STS		(1 << 1)
#define	 ALARM_INT0_STS		(1 << 0)
#define	THS_ALARM0_CTRL		0x50
#define	 ALARM_T_HOT_MASK	0xfff
#define	 ALARM_T_HOT_SHIFT	16
#define	 ALARM_T_HYST_MASK	0xfff
#define	 ALARM_T_HYST_SHIFT	0
#define	THS_SHUTDOWN0_CTRL	0x60
#define	 SHUT_T_HOT_MASK	0xfff
#define	 SHUT_T_HOT_SHIFT	16
#define	THS_FILTER		0x70
#define	THS_CALIB0		0x74
#define	THS_CALIB1		0x78
#define	THS_DATA0		0x80
#define	THS_DATA1		0x84
#define	THS_DATA2		0x88
#define	 DATA_MASK		0xfff

#define	A83T_ADC_ACQUIRE_TIME	0x17
#define	A83T_FILTER		0x4
#define	A83T_INTC		0x1000
#define	A83T_TEMP_BASE		2719000
#define	A83T_TEMP_MUL		1000
#define	A83T_TEMP_DIV		14186
#define	A83T_CLK_RATE		24000000

#define	A64_ADC_ACQUIRE_TIME	0x190
#define	A64_FILTER		0x6
#define	A64_INTC		0x18000
#define	A64_TEMP_BASE		2170000
#define	A64_TEMP_MUL		1000
#define	A64_TEMP_DIV		8560
#define	A64_CLK_RATE		4000000

#define	H3_ADC_ACQUIRE_TIME	0x3f
#define	H3_FILTER		0x6
#define	H3_INTC			0x191000
#define	H3_TEMP_BASE		1794000
#define	H3_TEMP_MUL		1000
#define	H3_TEMP_DIV		-8253
#define	H3_CLK_RATE		4000000

#define	TEMP_C_TO_K		273
#define	SENSOR_ENABLE_ALL	(SENSOR0_EN|SENSOR1_EN|SENSOR2_EN)
#define	SHUT_INT_ALL		(SHUT_INT0_STS|SHUT_INT1_STS|SHUT_INT2_STS)
#define	ALARM_INT_ALL		(ALARM_INT0_STS)

#define	MAX_SENSORS	3
#define	MAX_CF_LEVELS	64

#define	THROTTLE_ENABLE_DEFAULT	1

/* Enable thermal throttling */
static int aw_thermal_throttle_enable = THROTTLE_ENABLE_DEFAULT;
TUNABLE_INT("hw.aw_thermal.throttle_enable", &aw_thermal_throttle_enable);

struct aw_thermal_sensor {
	const char		*name;
	const char		*desc;
};

struct aw_thermal_config {
	struct aw_thermal_sensor	sensors[MAX_SENSORS];
	int				nsensors;
	uint64_t			clk_rate;
	uint32_t			adc_acquire_time;
	int				adc_cali_en;
	uint32_t			filter;
	uint32_t			intc;
	int				(*to_temp)(uint32_t);
	int				temp_base;
	int				temp_mul;
	int				temp_div;
	int				calib0, calib1;
	uint32_t			calib0_mask, calib1_mask;
};

static int
a83t_to_temp(uint32_t val)
{
	return ((A83T_TEMP_BASE - (val * A83T_TEMP_MUL)) / A83T_TEMP_DIV);
}

static const struct aw_thermal_config a83t_config = {
	.nsensors = 3,
	.sensors = {
		[0] = {
			.name = "cluster0",
			.desc = "CPU cluster 0 temperature",
		},
		[1] = {
			.name = "cluster1",
			.desc = "CPU cluster 1 temperature",
		},
		[2] = {
			.name = "gpu",
			.desc = "GPU temperature",
		},
	},
	.clk_rate = A83T_CLK_RATE,
	.adc_acquire_time = A83T_ADC_ACQUIRE_TIME,
	.adc_cali_en = 1,
	.filter = A83T_FILTER,
	.intc = A83T_INTC,
	.to_temp = a83t_to_temp,
	.calib0 = 1,
	.calib0_mask = 0xffffffff,
	.calib1 = 1,
	.calib1_mask = 0xffffffff,
};

static int
a64_to_temp(uint32_t val)
{
	return ((A64_TEMP_BASE - (val * A64_TEMP_MUL)) / A64_TEMP_DIV);
}

static const struct aw_thermal_config a64_config = {
	.nsensors = 3,
	.sensors = {
		[0] = {
			.name = "cpu",
			.desc = "CPU temperature",
		},
		[1] = {
			.name = "gpu1",
			.desc = "GPU temperature 1",
		},
		[2] = {
			.name = "gpu2",
			.desc = "GPU temperature 2",
		},
	},
	.clk_rate = A64_CLK_RATE,
	.adc_acquire_time = A64_ADC_ACQUIRE_TIME,
	.filter = A64_FILTER,
	.intc = A64_INTC,
	.to_temp = a64_to_temp,
};

static int
h3_to_temp(uint32_t val)
{
	return (((int)(val * H3_TEMP_MUL) - H3_TEMP_BASE) / H3_TEMP_DIV);
}

static const struct aw_thermal_config h3_config = {
	.nsensors = 1,
	.sensors = {
		[0] = {
			.name = "cpu",
			.desc = "CPU temperature",
		},
	},
	.clk_rate = H3_CLK_RATE,
	.adc_acquire_time = H3_ADC_ACQUIRE_TIME,
	.filter = H3_FILTER,
	.intc = H3_INTC,
	.to_temp = h3_to_temp,
	.calib0 = 1,
	.calib0_mask = 0xfff,
};

static struct ofw_compat_data compat_data[] = {
	{ "allwinner,sun8i-a83t-ts",	(uintptr_t)&a83t_config },
	{ "allwinner,sun8i-h3-ts",	(uintptr_t)&h3_config },
	{ "allwinner,sun50i-a64-ts",	(uintptr_t)&a64_config },
	{ NULL,				(uintptr_t)NULL }
};

#define	THS_CONF(d)		\
	(void *)ofw_bus_search_compatible((d), compat_data)->ocd_data

struct aw_thermal_softc {
	device_t			dev;
	struct resource			*res[2];
	struct aw_thermal_config	*conf;

	struct task			cf_task;
	int				throttle;
	int				min_freq;
	struct cf_level			levels[MAX_CF_LEVELS];
	eventhandler_tag		cf_pre_tag;
};

static struct resource_spec aw_thermal_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		0,	RF_ACTIVE },
	{ -1, 0 }
};

#define	RD4(sc, reg)		bus_read_4((sc)->res[0], (reg))
#define	WR4(sc, reg, val)	bus_write_4((sc)->res[0], (reg), (val))

static int
aw_thermal_init(struct aw_thermal_softc *sc)
{
	uint32_t calib0, calib1;
	int error;

	if (sc->conf->calib0 != 0 || sc->conf->calib1 != 0) {
		/* Read calibration settings from SRAM */
		error = aw_sid_read_tscalib(&calib0, &calib1);
		if (error != 0)
			return (error);

		calib0 &= sc->conf->calib0_mask;
		calib1 &= sc->conf->calib1_mask;

		/* Write calibration settings to thermal controller */
		if (sc->conf->calib0 != 0 && calib0 != 0)
			WR4(sc, THS_CALIB0, calib0);
		if (sc->conf->calib1 != 0 && calib1 != 0)
			WR4(sc, THS_CALIB1, calib1);
	}

	/* Configure ADC acquire time (CLK_IN/(N+1)) and enable sensors */
	WR4(sc, THS_CTRL1, ADC_CALI_EN);
	WR4(sc, THS_CTRL0, sc->conf->adc_acquire_time);
	WR4(sc, THS_CTRL2, sc->conf->adc_acquire_time << SENSOR_ACQ1_SHIFT);

	/* Enable average filter */
	WR4(sc, THS_FILTER, sc->conf->filter);

	/* Enable interrupts */
	WR4(sc, THS_INTS, RD4(sc, THS_INTS));
	WR4(sc, THS_INTC, sc->conf->intc | SHUT_INT_ALL | ALARM_INT_ALL);

	/* Enable sensors */
	WR4(sc, THS_CTRL2, RD4(sc, THS_CTRL2) | SENSOR_ENABLE_ALL);

	return (0);
}

static int
aw_thermal_gettemp(struct aw_thermal_softc *sc, int sensor)
{
	uint32_t val;

	val = RD4(sc, THS_DATA0 + (sensor * 4));

	return (sc->conf->to_temp(val) + TEMP_C_TO_K);
}

static int
aw_thermal_getshut(struct aw_thermal_softc *sc, int sensor)
{
	uint32_t val;

	val = RD4(sc, THS_SHUTDOWN0_CTRL + (sensor * 4));
	val = (val >> SHUT_T_HOT_SHIFT) & SHUT_T_HOT_MASK;

	return (sc->conf->to_temp(val) + TEMP_C_TO_K);
}

static int
aw_thermal_gethyst(struct aw_thermal_softc *sc, int sensor)
{
	uint32_t val;

	val = RD4(sc, THS_ALARM0_CTRL + (sensor * 4));
	val = (val >> ALARM_T_HYST_SHIFT) & ALARM_T_HYST_MASK;

	return (sc->conf->to_temp(val) + TEMP_C_TO_K);
}

static int
aw_thermal_getalarm(struct aw_thermal_softc *sc, int sensor)
{
	uint32_t val;

	val = RD4(sc, THS_ALARM0_CTRL + (sensor * 4));
	val = (val >> ALARM_T_HOT_SHIFT) & ALARM_T_HOT_MASK;

	return (sc->conf->to_temp(val) + TEMP_C_TO_K);
}

static int
aw_thermal_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct aw_thermal_softc *sc;
	int sensor, val;

	sc = arg1;
	sensor = arg2;

	val = aw_thermal_gettemp(sc, sensor);

	return sysctl_handle_opaque(oidp, &val, sizeof(val), req);
}

static void
aw_thermal_throttle(struct aw_thermal_softc *sc, int enable)
{
	device_t cf_dev;
	int count, error;

	if (enable == sc->throttle)
		return;

	if (enable != 0) {
		/* Set the lowest available frequency */
		cf_dev = devclass_get_device(devclass_find("cpufreq"), 0);
		if (cf_dev == NULL)
			return;
		count = MAX_CF_LEVELS;
		error = CPUFREQ_LEVELS(cf_dev, sc->levels, &count);
		if (error != 0 || count == 0)
			return;
		sc->min_freq = sc->levels[count - 1].total_set.freq;
		error = CPUFREQ_SET(cf_dev, &sc->levels[count - 1],
		    CPUFREQ_PRIO_USER);
		if (error != 0)
			return;
	}

	sc->throttle = enable;
}

static void
aw_thermal_cf_task(void *arg, int pending)
{
	struct aw_thermal_softc *sc;

	sc = arg;

	aw_thermal_throttle(sc, 1);
}

static void
aw_thermal_cf_pre_change(void *arg, const struct cf_level *level, int *status)
{
	struct aw_thermal_softc *sc;
	int temp_cur, temp_alarm;

	sc = arg;

	if (aw_thermal_throttle_enable == 0 || sc->throttle == 0 ||
	    level->total_set.freq == sc->min_freq)
		return;

	temp_cur = aw_thermal_gettemp(sc, 0);
	temp_alarm = aw_thermal_getalarm(sc, 0);

	if (temp_cur < temp_alarm)
		aw_thermal_throttle(sc, 0);
	else
		*status = ENXIO;
}

static void
aw_thermal_intr(void *arg)
{
	struct aw_thermal_softc *sc;
	device_t dev;
	uint32_t ints;

	dev = arg;
	sc = device_get_softc(dev);

	ints = RD4(sc, THS_INTS);
	WR4(sc, THS_INTS, ints);

	if ((ints & SHUT_INT_ALL) != 0) {
		device_printf(dev,
		    "WARNING - current temperature exceeds safe limits\n");
		shutdown_nice(RB_POWEROFF);
	}

	if ((ints & ALARM_INT_ALL) != 0)
		taskqueue_enqueue(taskqueue_thread, &sc->cf_task);
}

static int
aw_thermal_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (THS_CONF(dev) == NULL)
		return (ENXIO);

	device_set_desc(dev, "Allwinner Thermal Sensor Controller");
	return (BUS_PROBE_DEFAULT);
}

static int
aw_thermal_attach(device_t dev)
{
	struct aw_thermal_softc *sc;
	clk_t clk_ahb, clk_ths;
	hwreset_t rst;
	int i, error;
	void *ih;

	sc = device_get_softc(dev);
	clk_ahb = clk_ths = NULL;
	rst = NULL;
	ih = NULL;

	sc->conf = THS_CONF(dev);
	TASK_INIT(&sc->cf_task, 0, aw_thermal_cf_task, sc);

	if (bus_alloc_resources(dev, aw_thermal_spec, sc->res) != 0) {
		device_printf(dev, "cannot allocate resources for device\n");
		return (ENXIO);
	}

	if (clk_get_by_ofw_name(dev, 0, "ahb", &clk_ahb) == 0) {
		error = clk_enable(clk_ahb);
		if (error != 0) {
			device_printf(dev, "cannot enable ahb clock\n");
			goto fail;
		}
	}
	if (clk_get_by_ofw_name(dev, 0, "ths", &clk_ths) == 0) {
		error = clk_set_freq(clk_ths, sc->conf->clk_rate, 0);
		if (error != 0) {
			device_printf(dev, "cannot set ths clock rate\n");
			goto fail;
		}
		error = clk_enable(clk_ths);
		if (error != 0) {
			device_printf(dev, "cannot enable ths clock\n");
			goto fail;
		}
	}
	if (hwreset_get_by_ofw_idx(dev, 0, 0, &rst) == 0) {
		error = hwreset_deassert(rst);
		if (error != 0) {
			device_printf(dev, "cannot de-assert reset\n");
			goto fail;
		}
	}

	error = bus_setup_intr(dev, sc->res[1], INTR_TYPE_MISC | INTR_MPSAFE,
	    NULL, aw_thermal_intr, dev, &ih);
	if (error != 0) {
		device_printf(dev, "cannot setup interrupt handler\n");
		goto fail;
	}

	if (aw_thermal_init(sc) != 0)
		goto fail;

	for (i = 0; i < sc->conf->nsensors; i++)
		SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
		    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
		    OID_AUTO, sc->conf->sensors[i].name,
		    CTLTYPE_INT | CTLFLAG_RD,
		    sc, i, aw_thermal_sysctl, "IK0",
		    sc->conf->sensors[i].desc);

	if (bootverbose)
		for (i = 0; i < sc->conf->nsensors; i++) {
			device_printf(dev,
			    "#%d: alarm %dC hyst %dC shut %dC\n", i,
			    aw_thermal_getalarm(sc, i) - TEMP_C_TO_K,
			    aw_thermal_gethyst(sc, i) - TEMP_C_TO_K,
			    aw_thermal_getshut(sc, i) - TEMP_C_TO_K);
		}

	sc->cf_pre_tag = EVENTHANDLER_REGISTER(cpufreq_pre_change,
	    aw_thermal_cf_pre_change, sc, EVENTHANDLER_PRI_FIRST);

	return (0);

fail:
	if (ih != NULL)
		bus_teardown_intr(dev, sc->res[1], ih);
	if (rst != NULL)
		hwreset_release(rst);
	if (clk_ahb != NULL)
		clk_release(clk_ahb);
	if (clk_ths != NULL)
		clk_release(clk_ths);
	bus_release_resources(dev, aw_thermal_spec, sc->res);

	return (ENXIO);
}

static device_method_t aw_thermal_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		aw_thermal_probe),
	DEVMETHOD(device_attach,	aw_thermal_attach),

	DEVMETHOD_END
};

static driver_t aw_thermal_driver = {
	"aw_thermal",
	aw_thermal_methods,
	sizeof(struct aw_thermal_softc),
};

static devclass_t aw_thermal_devclass;

DRIVER_MODULE(aw_thermal, simplebus, aw_thermal_driver, aw_thermal_devclass,
    0, 0);
MODULE_VERSION(aw_thermal, 1);

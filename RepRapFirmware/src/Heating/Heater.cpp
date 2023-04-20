/*
 * Heater.cpp
 *
 *  Created on: 24 Jul 2019
 *      Author: David
 */

#include "Heater.h"
#include <Platform/RepRap.h>
#include <Platform/Platform.h>
#include "Heat.h"
#include "HeaterMonitor.h"
#include "Sensors/TemperatureSensor.h"
#include <GCodes/GCodes.h>
#include <GCodes/GCodeBuffer/GCodeBuffer.h>
#include <GCodes/GCodeException.h>

#if SUPPORT_OBJECT_MODEL

// Object model table and functions
// Note: if using GCC version 7.3.1 20180622 and lambda functions are used in this table, you must compile this file with option -std=gnu++17.
// Otherwise the table will be allocated in RAM instead of flash, which wastes too much RAM.

// Macro to build a standard lambda function that includes the necessary type conversions
#define OBJECT_MODEL_FUNC(...) OBJECT_MODEL_FUNC_BODY(Heater, __VA_ARGS__)
#define OBJECT_MODEL_FUNC_IF(...) OBJECT_MODEL_FUNC_IF_BODY(Heater, __VA_ARGS__)

constexpr ObjectModelArrayDescriptor Heater::monitorsArrayDescriptor =
{
	nullptr,
	[] (const ObjectModel *self, const ObjectExplorationContext&) noexcept -> size_t { return MaxMonitorsPerHeater; },
	[] (const ObjectModel *self, ObjectExplorationContext& context) noexcept -> ExpressionValue { return ExpressionValue(self, 1); }

};

constexpr ObjectModelTableEntry Heater::objectModelTable[] =
{
	// Within each group, these entries must be in alphabetical order
	// 0. Heater members
	{ "active",		OBJECT_MODEL_FUNC(self->GetActiveTemperature(), 1), 									ObjectModelEntryFlags::live },
	{ "current",	OBJECT_MODEL_FUNC(self->GetTemperature(), 1), 											ObjectModelEntryFlags::none },
	{ "max",		OBJECT_MODEL_FUNC(self->GetHighestTemperatureLimit(), 1), 								ObjectModelEntryFlags::none },
	{ "min",		OBJECT_MODEL_FUNC(self->GetLowestTemperatureLimit(), 1), 								ObjectModelEntryFlags::none },
	{ "model",		OBJECT_MODEL_FUNC((const FopDt *)&self->GetModel()),									ObjectModelEntryFlags::verbose },
	{ "monitors",	OBJECT_MODEL_FUNC_NOSELF(&monitorsArrayDescriptor), 									ObjectModelEntryFlags::none },
	{ "sensor",		OBJECT_MODEL_FUNC((int32_t)self->GetSensorNumber()), 									ObjectModelEntryFlags::none },
	{ "standby",	OBJECT_MODEL_FUNC(self->GetStandbyTemperature(), 1), 									ObjectModelEntryFlags::none },
	{ "state",		OBJECT_MODEL_FUNC(self->GetStatus().ToString()), 										ObjectModelEntryFlags::none },

	// 1. Heater.monitors[] members
	{ "action",		OBJECT_MODEL_FUNC_IF(self->monitors[context.GetLastIndex()].GetTrigger() != HeaterMonitorTrigger::Disabled,
										(int32_t)self->monitors[context.GetLastIndex()].GetAction()), 		ObjectModelEntryFlags::none },
	{ "condition",	OBJECT_MODEL_FUNC(self->monitors[context.GetLastIndex()].GetTriggerName()), 			ObjectModelEntryFlags::none },
	{ "limit",		OBJECT_MODEL_FUNC_IF(self->monitors[context.GetLastIndex()].GetTrigger() != HeaterMonitorTrigger::Disabled,
										self->monitors[context.GetLastIndex()].GetTemperatureLimit(), 1),	ObjectModelEntryFlags::none },
};

constexpr uint8_t Heater::objectModelTableDescriptor[] = { 2, 9, 3 };

DEFINE_GET_OBJECT_MODEL_TABLE(Heater)

#endif

// Static members of class Heater

float Heater::tuningPwm;								// the PWM to use, 0..1
float Heater::tuningTargetTemp;							// the target temperature
float Heater::tuningHysteresis;
float Heater::tuningFanPwm;

DeviationAccumulator Heater::tuningStartTemp;			// the temperature when we turned on the heater
DeviationAccumulator Heater::dHigh;
DeviationAccumulator Heater::dLow;
DeviationAccumulator Heater::tOn;
DeviationAccumulator Heater::tOff;
DeviationAccumulator Heater::heatingRate;
DeviationAccumulator Heater::coolingRate;
DeviationAccumulator Heater::tuningVoltage;				// sum of the voltage readings we take during the heating phase

uint32_t Heater::tuningBeginTime;						// when we started the tuning process
uint32_t Heater::lastOffTime;
uint32_t Heater::lastOnTime;
float Heater::peakTemp;									// max or min temperature
uint32_t Heater::peakTime;								// the time at which we recorded peakTemp
float Heater::afterPeakTemp;							// temperature after max from which we start timing the cooling rate
uint32_t Heater::afterPeakTime;							// the time at which we recorded afterPeakTemp
float Heater::lastCoolingRate;
FansBitmap Heater::tuningFans;
unsigned int Heater::tuningPhase;
uint8_t Heater::idleCyclesDone;

Heater::HeaterParameters Heater::fanOffParams, Heater::fanOnParams;

// Clear all the counters except tuning voltage and start temperature
/*static*/ void Heater::ClearCounters() noexcept
{
	dHigh.Clear();
	dLow.Clear();
	tOn.Clear();
	tOff.Clear();
	heatingRate.Clear();
	coolingRate.Clear();
}

Heater::Heater(unsigned int num) noexcept
	: tuned(false), heaterNumber(num), sensorNumber(-1), activeTemperature(0.0), standbyTemperature(0.0),
	  maxTempExcursion(DefaultMaxTempExcursion), maxHeatingFaultTime(DefaultMaxHeatingFaultTime),
	  active(false), modelSetByUser(false), monitorsSetByUser(false)
{
}

Heater::~Heater() noexcept
{
	for (HeaterMonitor& h : monitors)
	{
		h.Disable();
	}
}

void Heater::SetSensorNumber(int sn) noexcept
{
	if (sn != sensorNumber)
	{
		sensorNumber = sn;
	}
}

GCodeResult Heater::SetOrReportModel(unsigned int heater, GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException)
{
	bool seen = false;
	float heatingRate = model.GetHeatingRate();
	float td = model.GetDeadTime(),
		maxPwm = model.GetMaxPwm(),
		voltage = model.GetVoltage();
	float coolingRates[2] = { model.GetCoolingRateFanOff(), model.GetCoolingRateFanOn() };
	int32_t dontUsePid = model.UsePid() ? 0 : 1;
	int32_t inversionParameter = 0;

	// Get the cooling time constant(s) first
	float timeConstants[2];
	size_t numValues = 2;
	if (gb.TryGetFloatArray('C', numValues, timeConstants, reply, seen, true))
	{
		return GCodeResult::error;
	}
	else if (seen)
	{
		coolingRates[0] = 1.0/timeConstants[0];
		coolingRates[1] = 1.0/timeConstants[1];
	}

	if (gb.Seen('R'))
	{
		// New style heater model. R = heating rate, C[2] = cooling rates
		seen = true;
		heatingRate = gb.GetFValue();
	}
	else if (gb.Seen('A'))
	{
		// Old style heating model. A = gain, C = cooling time constant
		seen = true;
		const float gain = gb.GetFValue();
		heatingRate = gain * coolingRates[0];
	}
	gb.TryGetFValue('D', td, seen);
	gb.TryGetIValue('B', dontUsePid, seen);
	gb.TryGetFValue('S', maxPwm, seen);
	gb.TryGetFValue('V', voltage, seen);
	gb.TryGetIValue('I', inversionParameter, seen);

	if (seen)
	{
		// Set the model
		const bool inverseTemperatureControl = (inversionParameter == 1 || inversionParameter == 3);
		const GCodeResult rslt = SetModel(heatingRate, coolingRates[0], coolingRates[1], td, maxPwm, voltage, dontUsePid == 0, inverseTemperatureControl, reply);
		if (rslt <= GCodeResult::warning)
		{
			modelSetByUser = true;
		}
		return rslt;
	}

	// Just report the model
	if (!model.IsEnabled())
	{
		reply.printf("Heater %u is disabled because its model is undefined", heater);
	}
	else
	{
		const char* const mode = (!model.UsePid()) ? "bang-bang"
									: (model.ArePidParametersOverridden()) ? "custom PID"
										: "PID";
		reply.printf("Heater %u model: heating rate %.3f, cooling time constant %.1f", heater, (double)model.GetHeatingRate(), (double)model.GetTimeConstantFanOff());
		if (model.GetCoolingRateChangeFanOn() > 0.0)
		{
			reply.catf("/%.1f", (double)model.GetTimeConstantFanOn());
		}
		reply.catf(", dead time %.2f, max PWM %.2f, calibration voltage %.1f, mode %s", (double)model.GetDeadTime(), (double)model.GetMaxPwm(), (double)model.GetVoltage(), mode);
		if (model.IsInverted())
		{
			reply.cat(", inverted control");
		}
		if (model.UsePid())
		{
			M301PidParameters params = model.GetM301PidParameters(false);
			reply.catf("\nComputed PID parameters: setpoint change: P%.1f, I%.3f, D%.1f", (double)params.kP, (double)params.kI, (double)params.kD);
			params = model.GetM301PidParameters(true);
			reply.catf(", load change: P%.1f, I%.3f, D%.1f", (double)params.kP, (double)params.kI, (double)params.kD);
		}
	}
	return GCodeResult::ok;
}

// Set the process model returning true if successful
GCodeResult Heater::SetModel(float hr, float coolingRateFanOff, float coolingRateFanOn, float td, float maxPwm, float voltage, bool usePid, bool inverted, const StringRef& reply) noexcept
{
	GCodeResult rslt;
	if (model.SetParameters(hr, coolingRateFanOff, coolingRateFanOn, td, maxPwm, GetHighestTemperatureLimit(), voltage, usePid, inverted))
	{
		if (model.IsEnabled())
		{
			rslt = UpdateModel(reply);
			if (rslt == GCodeResult::ok)
			{
				const float predictedMaxTemp = hr/coolingRateFanOff + NormalAmbientTemperature;
				const float noWarnTemp = (GetHighestTemperatureLimit() - NormalAmbientTemperature) * 1.5 + 50.0;		// allow 50% extra power plus enough for an extra 50C
				if (predictedMaxTemp > noWarnTemp)
				{
					reply.printf("Heater %u appears to be over-powered. If left on at full power, its temperature is predicted to reach %dC", GetHeaterNumber(), (int)predictedMaxTemp);
					rslt = GCodeResult::warning;
				}
			}
		}
		else
		{
			ResetHeater();
			tuned = false;
			rslt = GCodeResult::ok;
		}
	}
	else
	{
		reply.copy("bad model parameters");
		rslt = GCodeResult::error;
	}

	reprap.HeatUpdated();
	return rslt;
}

// Start an auto tune cycle for this heater
GCodeResult Heater::StartAutoTune(GCodeBuffer& gb, const StringRef& reply, FansBitmap fans) THROWS(GCodeException)
{
	// Get the target temperature (required)
	gb.MustSee('S');
	const float targetTemp = gb.GetFValue();

	if (!GetModel().IsEnabled())
	{
		reply.printf("heater %u cannot be auto tuned while it is disabled", GetHeaterNumber());
		return GCodeResult::error;
	}

	const float limit = GetHighestTemperatureLimit();
	if (targetTemp >= limit)
	{
		reply.printf("heater %u target temperature must be below the temperature limit for this heater (%.1fC)", GetHeaterNumber(), (double)limit);
		return GCodeResult::error;
	}

	TemperatureError err;
	const float currentTemp = reprap.GetHeat().GetSensorTemperature(GetSensorNumber(), err);
	if (err != TemperatureError::success)
	{
		reply.printf("heater %u reported error '%s' at start of auto tuning", GetHeaterNumber(), TemperatureErrorString(err));
		return GCodeResult::error;
	}

	const bool seenA = gb.Seen('A');
	const float ambientTemp = (seenA) ? gb.GetFValue() : currentTemp;
	if (ambientTemp + 20 >= targetTemp)
	{
		reply.printf("Target temperature must be at least 20C above ambient temperature");
	}

	// Get abd store the optional parameters
	tuningTargetTemp = targetTemp;
	tuningFans = fans;
	tuningPwm = (gb.Seen('P')) ? gb.GetLimitedFValue('P', 0.1, 1.0) : GetModel().GetMaxPwm();
	tuningHysteresis = (gb.Seen('Y')) ? gb.GetLimitedFValue('Y', 1.0, 20.0) : DefaultTuningHysteresis;
	tuningFanPwm = (gb.Seen('F')) ? gb.GetLimitedFValue('F', 0.1, 1.0) : 1.0;

	const GCodeResult rslt = StartAutoTune(reply, seenA, ambientTemp);
	if (rslt == GCodeResult::ok)
	{
		reply.printf("Auto tuning heater %u using target temperature %.1f" DEGREE_SYMBOL "C and PWM %.2f - do not leave printer unattended",
						GetHeaterNumber(), (double)targetTemp, (double)tuningPwm);
	}
	return rslt;
}

const char *const Heater::TuningPhaseText[] =
{
	"checking temperature is stable",
	"heating up",
	"heating system settling",
	"tuning with fan off",
#if TUNE_WITH_HALF_FAN
	"tuning with 50% fan",
#endif
	"tuning with fan on"
};

// Get the auto tune status or last result
void Heater::GetAutoTuneStatus(const StringRef& reply) const noexcept
{
	if (GetStatus() == HeaterStatus::tuning)
	{
		// Phases are: 1 = stabilising, 2 = heating, 3 = settling, 4 = cycling with fan off, 5 = cycling with fan on
		const unsigned int numPhases = (tuningFans.IsEmpty()) ? 4 : ARRAY_SIZE(TuningPhaseText);
		reply.printf("Heater %u is being tuned, phase %u of %u, %s", GetHeaterNumber(), tuningPhase + 1, numPhases, TuningPhaseText[tuningPhase]);
	}
	else if (tuned)
	{
		reply.printf("Heater %u tuning succeeded, use M307 H%u to see result", GetHeaterNumber(), GetHeaterNumber());
	}
	else
	{
		reply.printf("Heater %u tuning failed", GetHeaterNumber());
	}
}

// Tell the user what's happening, called after the tuning phase has been updated
void Heater::ReportTuningUpdate() noexcept
{
	if (tuningPhase < ARRAY_SIZE(TuningPhaseText))
	{
		reprap.GetPlatform().MessageF(GenericMessage, "Auto tune starting phase %u, %s\n", tuningPhase + 1, TuningPhaseText[tuningPhase]);
	}
}

void Heater::CalculateModel(HeaterParameters& params) noexcept
{
	if (reprap.Debug(moduleHeat))
	{
#define PLUS_OR_MINUS "\xC2\xB1"
		reprap.GetPlatform().MessageF(GenericMessage,
										"tOn %ld" PLUS_OR_MINUS "%ld, tOff %ld" PLUS_OR_MINUS "%ld,"
										" dHigh %ld" PLUS_OR_MINUS "%ld, dLow %ld" PLUS_OR_MINUS "%ld,"
										" R %.3f" PLUS_OR_MINUS "%.3f, C %.3f" PLUS_OR_MINUS "%.3f,"
#if HAS_VOLTAGE_MONITOR
										" V %.1f" PLUS_OR_MINUS "%.1f,"
#endif
										" cycles %u\n",
										lrintf(tOn.GetMean()), lrintf(tOn.GetDeviation()),
										lrintf(tOff.GetMean()), lrintf(tOff.GetDeviation()),
										lrintf(dHigh.GetMean()), lrintf(dHigh.GetDeviation()),
										lrintf(dLow.GetMean()), lrintf(dLow.GetDeviation()),
										(double)heatingRate.GetMean(), (double)heatingRate.GetDeviation(),
										(double)coolingRate.GetMean(), (double)coolingRate.GetDeviation(),
#if HAS_VOLTAGE_MONITOR
										(double)tuningVoltage.GetMean(), (double)tuningVoltage.GetDeviation(),
#endif
										coolingRate.GetNumSamples()
									 );
	}

	const float cycleTime = tOn.GetMean() + tOff.GetMean();		// in milliseconds
	const float averageTemperatureRiseHeating = tuningTargetTemp - 0.5 * (tuningHysteresis - TuningPeakTempDrop) - tuningStartTemp.GetMean();
	const float averageTemperatureRiseCooling = tuningTargetTemp - TuningPeakTempDrop - 0.5 * tuningHysteresis - tuningStartTemp.GetMean();
	params.deadTime = (((dHigh.GetMean() * tOff.GetMean()) + (dLow.GetMean() * tOn.GetMean())) * MillisToSeconds)/cycleTime;	// in seconds
	params.coolingRate = coolingRate.GetMean()/averageTemperatureRiseCooling;			// in seconds
	params.heatingRate = (heatingRate.GetMean() + (coolingRate.GetMean() * averageTemperatureRiseHeating/averageTemperatureRiseCooling)) / tuningPwm;
	params.numCycles = dHigh.GetNumSamples();
}

void Heater::SetAndReportModel(bool usingFans) noexcept
{
	const float hRate = (usingFans) ? (fanOffParams.heatingRate + fanOnParams.heatingRate) * 0.5 : fanOffParams.heatingRate;
	const float deadTime = (usingFans) ? (fanOffParams.deadTime + fanOnParams.deadTime) * 0.5 : fanOffParams.deadTime;

	float fanOnCoolingRate = fanOffParams.coolingRate;
	if (usingFans)
	{
		// Sometimes the print cooling fan makes no difference to the cooling rate. The SetModel call will fail if the rate with fan on is lower than the rate with fan off.
		if (fanOnParams.coolingRate > fanOffParams.coolingRate)
		{
			fanOnCoolingRate = fanOffParams.coolingRate + (fanOnParams.coolingRate - fanOffParams.coolingRate)/tuningFanPwm;
		}
		else
		{
			reprap.GetPlatform().Message(WarningMessage, "Turning on the print cooling fan did not increase hot end cooling. Check that the correct fan has been configured.\n");
		}
	}

	String<StringLength256> str;
	const GCodeResult rslt = SetModel(	hRate,
										fanOffParams.coolingRate, fanOnCoolingRate,
										deadTime,
										tuningPwm,
#if HAS_VOLTAGE_MONITOR
										tuningVoltage.GetMean(),
#else
										0.0,
#endif
										true, false, str.GetRef());
	if (rslt == GCodeResult::ok || rslt == GCodeResult::warning)
	{
		tuned = true;
		str.printf("Auto tuning heater %u completed after %u idle and %u tuning cycles in %" PRIu32 " seconds. This heater needs the following M307 command:\n"
					" M307 H%u B0 R%.3f C%.1f",
					GetHeaterNumber(),
					idleCyclesDone,
					(usingFans) ? fanOffParams.numCycles + fanOnParams.numCycles : fanOffParams.numCycles,
					(millis() - tuningBeginTime)/(uint32_t)SecondsToMillis,
					GetHeaterNumber(), (double)GetModel().GetHeatingRate(), (double)(1.0/GetModel().GetCoolingRateFanOff())
				  );
		if (usingFans)
		{
			str.catf(":%.1f", (double)(1.0/GetModel().GetCoolingRateFanOn()));
		}
		str.catf(" D%.2f S%.2f V%.1f\n", (double)GetModel().GetDeadTime(), (double)GetModel().GetMaxPwm(), (double)GetModel().GetVoltage());
		reprap.GetPlatform().Message(LoggedGenericMessage, str.c_str());
		if (reprap.GetGCodes().SawM501InConfigFile())
		{
			reprap.GetPlatform().Message(GenericMessage, "Send M500 to save this command in config-override.g\n");
		}
		else
		{
			reprap.GetPlatform().MessageF(GenericMessage, "Edit the M307 H%u command in config.g to match this. Omit the V parameter if the heater is not powered from VIN.\n", GetHeaterNumber());
		}
	}
	else
	{
		reprap.GetPlatform().MessageF(WarningMessage, "Auto tune of heater %u failed due to bad curve fit (R=%.3f, 1/C=%.4f:%.4f, D=%.1f)\n",
										GetHeaterNumber(), (double)hRate,
										(double)fanOffParams.coolingRate, (double)fanOnCoolingRate,
										(double)fanOffParams.deadTime);
	}
}

GCodeResult Heater::SetFaultDetectionParameters(float pMaxTempExcursion, float pMaxFaultTime, const StringRef& reply) noexcept
{
	maxTempExcursion = pMaxTempExcursion;
	maxHeatingFaultTime = pMaxFaultTime;
	const GCodeResult rslt = UpdateFaultDetectionParameters(reply);
	reprap.HeatUpdated();
	return rslt;
}

// Process M143 for this heater
GCodeResult Heater::ConfigureMonitor(GCodeBuffer &gb, const StringRef &reply) THROWS(GCodeException)
{
	// Get any parameters that have been provided
	uint32_t index = 0;
	bool seenP = false;
	gb.TryGetLimitedUIValue('P', index, seenP, MaxMonitorsPerHeater);

	bool seenSensor = false;
	uint32_t monitoringSensor = GetSensorNumber();
	gb.TryGetLimitedUIValue('T', monitoringSensor, seenSensor, MaxSensors);

	const bool seenAction = gb.Seen('A');
	const HeaterMonitorAction action = (seenAction)
										? static_cast<HeaterMonitorAction>(gb.GetLimitedUIValue('A', (unsigned int)MaxHeaterMonitorAction + 1))
											: HeaterMonitorAction::GenerateFault;

	const bool seenCondition = gb.Seen('C');
	const HeaterMonitorTrigger trigger = (seenCondition)
											? static_cast<HeaterMonitorTrigger>(gb.GetLimitedIValue('C', -1, (int)MaxHeaterMonitorTrigger))
												: HeaterMonitorTrigger::TemperatureExceeded;

	const bool seenLimit = gb.Seen('S');
	const float limit = (seenLimit) ? gb.GetFValue() : monitors[index].GetTemperatureLimit();
	if (limit <= BadLowTemperature || limit >= BadErrorTemperature)
	{
		reply.copy("Invalid temperature limit");
		return GCodeResult::error;
	}

	if (seenSensor || seenLimit || seenAction || seenCondition)
	{
		monitors[index].Set(monitoringSensor, limit, action, trigger);
		const GCodeResult rslt = UpdateHeaterMonitors(reply);
		if (rslt <= GCodeResult::warning)
		{
			monitorsSetByUser = true;
		}
		reprap.HeatUpdated();
		return rslt;
	}

	// Else we are reporting on one or all of the monitors
	if (seenP)
	{
		monitors[index].Report(heaterNumber, index, reply);
	}
	else
	{
		for (size_t i = 0; i < MaxMonitorsPerHeater; ++i)
		{
			monitors[i].Report(heaterNumber, i, reply);
		}
	}
	return GCodeResult::ok;
}

float Heater::GetHighestTemperatureLimit() const noexcept
{
	float limit = BadErrorTemperature;
	for (const HeaterMonitor& prot : monitors)
	{
		if (prot.GetTrigger() == HeaterMonitorTrigger::TemperatureExceeded)
		{
			const float t = prot.GetTemperatureLimit();
			if (limit == BadErrorTemperature || t > limit)
			{
				limit = t;
			}
		}
	}
	return limit;
}

float Heater::GetLowestTemperatureLimit() const noexcept
{
	float limit = ABS_ZERO;
	for (const HeaterMonitor& prot : monitors)
	{
		if (prot.GetTrigger() == HeaterMonitorTrigger::TemperatureTooLow)
		{
			const float t = prot.GetTemperatureLimit();
			if (limit == ABS_ZERO || t < limit)
			{
				limit = t;
			}
		}
	}
	return limit;
}

HeaterStatus Heater::GetStatus() const noexcept
{
	const HeaterMode mode = GetMode();
	return (mode == HeaterMode::fault) ? HeaterStatus::fault
			: (mode == HeaterMode::offline) ? HeaterStatus::offline
				: (mode == HeaterMode::off) ? HeaterStatus::off
					: (mode >= HeaterMode::tuning0) ? HeaterStatus::tuning
						: (active) ? HeaterStatus::active
							: HeaterStatus::standby;
}

const char* Heater::GetSensorName() const noexcept
{
	const auto sensor = reprap.GetHeat().FindSensor(sensorNumber);
	return (sensor.IsNotNull()) ? sensor->GetSensorName() : nullptr;
}

GCodeResult Heater::Activate(const StringRef& reply) noexcept
{
	if (GetMode() != HeaterMode::fault)
	{
		active = true;
		return SwitchOn(reply);
	}
	reply.printf("Can't activate heater %u while in fault state", heaterNumber);
	return GCodeResult::error;
}

void Heater::Standby() noexcept
{
	if (GetMode() != HeaterMode::fault)
	{
		active = false;
		String<1> dummy;
		(void)SwitchOn(dummy.GetRef());
	}
}

void Heater::SetTemperature(float t, bool activeNotStandby) THROWS(GCodeException)
{
	if (t > GetHighestTemperatureLimit())
	{
		throw GCodeException(-1, -1, "Temperature too high for heater %" PRIu32, (uint32_t)GetHeaterNumber());
	}
	else if (t < GetLowestTemperatureLimit())
	{
		throw GCodeException(-1, -1, "Temperature too low for heater %" PRIu32, (uint32_t)GetHeaterNumber());
	}
	else
	{
		((activeNotStandby) ? activeTemperature : standbyTemperature) = t;
		if (GetMode() > HeaterMode::suspended && active == activeNotStandby)
		{
			String<1> dummy;
			(void)SwitchOn(dummy.GetRef());
		}
	}
}

// This is called when config.g is about to be re-run
void Heater::ClearModelAndMonitors() noexcept
{
	model.Clear();
	for (HeaterMonitor& hm : monitors)
	{
		hm.Disable();
	}
	modelSetByUser = monitorsSetByUser = false;
}

// This is called when this heater is declared to be a bed or chamber heater using M140 or M141
void Heater::SetAsToolHeater() noexcept
{
	if (!modelSetByUser)
	{
		model.SetDefaultToolParameters();
	}
	if (!monitorsSetByUser && sensorNumber >= 0 && sensorNumber < (int)MaxSensors)
	{
		monitors[0].Set(sensorNumber, DefaultHotEndTemperatureLimit, HeaterMonitorAction::GenerateFault, HeaterMonitorTrigger::TemperatureExceeded);
	}
}

// This is called when a heater is declared to be a tool heater using M563
void Heater::SetAsBedOrChamberHeater() noexcept
{
	if (!modelSetByUser)
	{
		model.SetDefaultBedOrChamberParameters();
	}
	if (!monitorsSetByUser &&sensorNumber >= 0 && sensorNumber < (int)MaxSensors)
	{
		monitors[0].Set(sensorNumber, DefaultBedTemperatureLimit, HeaterMonitorAction::GenerateFault, HeaterMonitorTrigger::TemperatureExceeded);
	}
}

// End

/****************************************************************************************************

RepRapFirmware - G Codes

This class interprets G Codes from one or more sources, and calls the functions in Move, Heat etc
that drive the machine to do what the G Codes command.

-----------------------------------------------------------------------------------------------------

Version 0.1

13 February 2013

Adrian Bowyer
RepRap Professional Ltd
http://reprappro.com

Licence: GPL

****************************************************************************************************/

#ifndef GCODES_H
#define GCODES_H

#include "RepRapFirmware.h"
#include <Platform/RepRap.h>			// for type ResponseSource
#include "ObjectTracker.h"
#include <Movement/RawMove.h>
#include <Libraries/sha1/sha1.h>
#include <Platform/Platform.h>		// for type EndStopHit
#include "GCodeChannel.h"
#include "GCodeInput.h"
#include "Trigger.h"
#include <Tools/Filament.h>
#include <FilamentMonitors/FilamentMonitor.h>
#include "RestorePoint.h"
#include "StraightProbeSettings.h"
#include <Movement/BedProbing/Grid.h>

const char feedrateLetter = 'F';						// GCode feedrate
const char extrudeLetter = 'E'; 						// GCode extrude

// Bits for T-code P-parameter to specify which macros are supposed to be run
constexpr uint8_t TFreeBit = 1u << 0;
constexpr uint8_t TPreBit = 1u << 1;
constexpr uint8_t TPostBit = 1u << 2;
constexpr uint8_t DefaultToolChangeParam = TFreeBit | TPreBit | TPostBit;

// Machine type enumeration. The numeric values must be in the same order as the corresponding M451..M453 commands.
enum class MachineType : uint8_t
{
	fff = 0,
	laser = 1,
	cnc = 2,
    LatheMode = 3,
	MillingMode = 4,
	DefaultMode = 5,
	HMCMode = 6,
	FiveAxisBCMode = 7,
	LatheMode_Tailstock = 8,
	MillingMode_Tailstock = 9,
	HMCMode_Tailstock = 10,
	FiveAxisBCMode_Tailstock = 11,
};

enum class PauseReason
{
	user,			// M25 command received
	gcode,			// M25 or M226 command encountered in the file being printed
	filamentChange,	// M600 command
	trigger,		// external switch
	heaterFault,	// heater fault detected
	filamentError,	// filament monitor
#if HAS_SMART_DRIVERS
	stall,			// motor stall detected
#endif
};

// Keep this in sync with PrintStopReason in Linux/MessageFormats.h
enum class StopPrintReason
{
	normalCompletion,
	userCancelled,
	abort
};

enum class PauseState : uint8_t
{
	// Do not change the order of these! We rely on notPaused < pausing < { paused, resuming}
	notPaused = 0,
	pausing,
	paused,
	resuming
};

struct M585Settings
{
	size_t axisNumber;			// the axis we are moving
	float feedRate;
	float probingLimit;
	float offset;
	bool useProbe;				// true if using a probe (M585 only because M675 always uses a probe)
};

struct M675Settings
{
	size_t axisNumber;			// the axis we are moving
	float feedRate;
	float backoffDistance;		// back off distance
	float minDistance;			// the position we reached when probing towards minimum
};

class LinuxInterface;

// The GCode interpreter

class GCodes
{
public:
	//PrintGcodeHistory
	bool isPrintGcodeHistory = false;
	//M800 lock use
	bool bM800 = false;
	//M802 lock use
	bool bM802 = false;
	bool bM799_1 = true;
	bool bM799_2 = true;

	//G234 TCPC
	bool G234_active = false;
	//G234 TCPC
	bool G235_active = false;
	float G235_radius = 0.0;
	float G235_gap = 0.0;
	float HomePosition[6];      // Home position
	//float BacklashDistance[5]={ 0.0 };     // (mm)
	float BacklashDistance[6]={ 0.0 };     // (mm)jimmy
	//float RestoreBeforeDistance[5]={ -9999.0 };     // (mm)
	float RestoreBeforeDistance[6]={ -9999.0 };     // (mm)
	size_t X_index = 0, Y_index = 0, Z_index = 0;
	size_t A_index = 0, B_index = 0, C_index = 0, D_index = 0;
	size_t U_index = 0, V_index = 0;
	bool IsDiameterMode = false; // only for lathe mode
	bool IsFinished = false;
	bool IsLock = true;

	//time
	uint32_t StartTime;
	uint32_t NowTime;

	GCodes(Platform& p) noexcept;
	void Spin() noexcept;														// Called in a tight loop to make this class work
	void Init() noexcept;														// Set it up
	void Exit() noexcept;														// Shut it down
	void Reset() noexcept;														// Reset some parameter to defaults
	bool ReadMove(RawMove& m) noexcept;											// Called by the Move class to get a movement set by the last G Code
	void ClearMove() noexcept;
#if HAS_MASS_STORAGE
	bool QueueFileToPrint(const char* fileName, const StringRef& reply) noexcept;	// Open a file of G Codes to run
#endif
	void AbortPrint(GCodeBuffer& gb) noexcept;									// Cancel any print in progress
	void GetCurrentCoordinates(const StringRef& s) const noexcept;				// Write where we are into a string
	bool DoingFileMacro() const noexcept;										// Is a macro file being processed?
	bool WaitingForAcknowledgement() const noexcept;							// Is an input waiting for a message to be acknowledged?

	FilePosition GetFilePosition() const noexcept;								// Return the current position of the file being printed in bytes
	void Diagnostics(MessageType mtype) noexcept;								// Send helpful information out

	bool RunConfigFile(const char* fileName) noexcept;							// Start running the config file
	bool IsTriggerBusy() const noexcept;										// Return true if the trigger G-code buffer is busy running config.g or a trigger file

	bool IsAxisHomed(unsigned int axis) const noexcept							// Has the axis been homed?
		{ return axesVirtuallyHomed.IsBitSet(axis); }
	void SetAxisIsHomed(unsigned int axis) noexcept;							// Tell us that the axis is now homed
	void SetAxisNotHomed(unsigned int axis) noexcept;							// Tell us that the axis is not homed
	void SetAllAxesNotHomed() noexcept;											// Flag all axes as not homed

	float GetSpeedFactor() const noexcept { return speedFactor; }				// Return the current speed factor as a fraction
	float GetExtrusionFactor(size_t extruder) noexcept;							// Return the current extrusion factor for the specified extruder
#if SUPPORT_12864_LCD
	void SetSpeedFactor(float factor) noexcept;									// Set the speed factor
	void SetExtrusionFactor(size_t extruder, float factor) noexcept;			// Set an extrusion factor for the specified extruder
#endif

	float GetRawExtruderTotalByDrive(size_t extruder) const noexcept;			// Get the total extrusion since start of print, for one drive
	float GetTotalRawExtrusion() const noexcept { return rawExtruderTotal; }	// Get the total extrusion since start of print, all drives
	float GetServoStatus(size_t axis) const noexcept
		pre(axis < maxAxes);
	float GetTotalBabyStepOffset(size_t axis) const noexcept
		pre(axis < maxAxes);
	float GetUserCoordinate(size_t axis) const noexcept;						// Get the current user coordinate in the current workspace coordinate system

	bool CheckNetworkCommandAllowed(GCodeBuffer& gb, const StringRef& reply, GCodeResult& result) noexcept;
#if HAS_NETWORKING
	NetworkGCodeInput *GetHTTPInput() const noexcept { return httpInput; }
	NetworkGCodeInput *GetTelnetInput() const noexcept { return telnetInput; }
#endif

	PauseState GetPauseState() const noexcept { return pauseState; }
	bool IsFlashing() const noexcept { return isFlashing; }						// Is a new firmware binary going to be flashed?
	bool IsFlashingPanelDue() const noexcept { return isFlashingPanelDue; }

	bool IsReallyPrinting() const noexcept;										// Return true if we are printing from SD card and not pausing, paused or resuming
	bool IsReallyPrintingOrResuming() const noexcept;
	bool IsSimulating() const noexcept { return simulationMode != 0; }
	bool IsDoingToolChange() const noexcept { return doingToolChange; }
	bool IsHeatingUp() const noexcept;											// Return true if the SD card print is waiting for a heater to reach temperature
	bool IsRunningConfigFile() const noexcept { return runningConfigFile; }
	bool SawM501InConfigFile() const noexcept { return m501SeenInConfigFile; }

	uint32_t GetLastDuration() const noexcept { return lastDuration; }			// The most recent print time or simulated time
	float GetSimulationTime() const noexcept { return simulationTime; }

	bool AllAxesAreHomed() const noexcept;										// Return true if all axes are homed

	void MoveStoppedByZProbe() noexcept { zProbeTriggered = true; }				// Called from the step ISR when the Z probe is triggered, causing the move to be aborted

	size_t GetTotalAxes() const noexcept { return numTotalAxes; }
	size_t GetVisibleAxes() const noexcept { return numVisibleAxes; }
	size_t GetNumExtruders() const noexcept { return numExtruders; }

	const char* GetMachineModeString() const noexcept;							// Get the name of the current machine mode

	void FilamentError(size_t extruder, FilamentSensorStatus fstat) noexcept;
	void HandleHeaterFault() noexcept;											// Respond to a heater fault

#if HAS_VOLTAGE_MONITOR
	bool LowVoltagePause() noexcept;
	bool LowVoltageResume() noexcept;
#endif

#if HAS_SMART_DRIVERS
	bool PauseOnStall(DriversBitmap stalledDrivers) noexcept;
	bool ReHomeOnStall(DriversBitmap stalledDrivers) noexcept;
#endif

	const char *GetAxisLetters() const noexcept { return axisLetters; }			// Return a null-terminated string of axis letters indexed by drive
	size_t GetAxisNumberForLetter(const char axisLetter) const noexcept;
	MachineType GetMachineType() const noexcept { return machineType; }
	bool LockMovementAndWaitForStandstill(GCodeBuffer& gb) noexcept;			// Lock movement and wait for pending moves to finish

#if SUPPORT_12864_LCD
	bool ProcessCommandFromLcd(const char *cmd) noexcept;						// Process a GCode command from the 12864 LCD returning true if the command was accepted
	float GetItemCurrentTemperature(unsigned int itemNumber) const noexcept;
	float GetItemActiveTemperature(unsigned int itemNumber) const noexcept;
	float GetItemStandbyTemperature(unsigned int itemNumber) const noexcept;
	void SetItemActiveTemperature(unsigned int itemNumber, float temp) noexcept;
	void SetItemStandbyTemperature(unsigned int itemNumber, float temp) noexcept;
#endif

	float GetMappedFanSpeed() const noexcept { return lastDefaultFanSpeed; }	// Get the mapped fan speed
	void SetMappedFanSpeed(float f) noexcept;									// Set the speeds of fans mapped for the current tool
	void HandleReply(GCodeBuffer& gb, GCodeResult rslt, const char *reply) noexcept;	// Handle G-Code replies
	void EmergencyStop() noexcept;												// Cancel everything
	bool AtxPowerControlled() const noexcept { return atxPowerControlled; }

	const GridDefinition& GetDefaultGrid() const { return defaultGrid; };		// Get the default grid definition
	bool AssignGrid(const char axesLetters[2], const float axis0Range[2], const float axis1Range[2], float radius, float spacing[2]) noexcept;	// Assign the heightmap using the given parameters
	void ActivateHeightmap(bool activate) noexcept;								// (De-)Activate the height map

	int GetNewToolNumber() const noexcept { return newToolNumber; }
	size_t GetCurrentZProbeNumber() const noexcept { return currentZProbeNumber; }

	// These next two are public because they are used by class LinuxInterface
	void UnlockAll(const GCodeBuffer& gb) noexcept;								// Release all locks
	GCodeBuffer *GetGCodeBuffer(GCodeChannel channel) const noexcept { return gcodeSources[channel.ToBaseType()]; }

#if HAS_MASS_STORAGE
	GCodeResult StartSDTiming(GCodeBuffer& gb, const StringRef& reply) noexcept;	// Start timing SD card file writing
#endif

	void SavePosition(RestorePoint& rp, const GCodeBuffer& gb) const noexcept;		// Save position etc. to a restore point
	void StartToolChange(GCodeBuffer& gb, int toolNum, uint8_t param) noexcept;

	unsigned int GetWorkplaceCoordinateSystemNumber() const noexcept { return currentCoordinateSystem + 1; }

	// This function is called by other functions to account correctly for workplace coordinates
	float GetWorkplaceOffset(size_t axis) const noexcept
	{
		return workplaceCoordinates[currentCoordinateSystem][axis];
	}

#if SUPPORT_OBJECT_MODEL
	float GetWorkplaceOffset(size_t axis, size_t workplaceNumber) const noexcept
	{
		return workplaceCoordinates[workplaceNumber][axis];
	}

	size_t GetNumInputs() const noexcept { return NumGCodeChannels; }
	const GCodeBuffer* GetInput(size_t n) const noexcept { return gcodeSources[n]; }
	const GCodeBuffer* GetInput(GCodeChannel n) const noexcept { return gcodeSources[n.RawValue()]; }
	const ObjectTracker *GetBuildObjects() const noexcept { return &buildObjects; }
	const RestorePoint *GetRestorePoint(size_t n) const pre(n < NumRestorePoints) { return &numberedRestorePoints[n]; }
	float GetVirtualExtruderPosition() const noexcept { return virtualExtruderPosition; }
	int GetMPGMODE() const noexcept { return MPG_MODE; }

# if HAS_VOLTAGE_MONITOR
	const char *GetPowerFailScript() const noexcept { return powerFailScript; }
# endif

# if SUPPORT_LASER
	// Return laser PWM in 0..1
	float GetLaserPwm() const noexcept
	{
		return (float)moveBuffer.laserPwmOrIoBits.laserPwm * (1.0/65535);
	}
# endif
#endif

#if SUPPORT_REMOTE_COMMANDS
	void SwitchToExpansionMode() noexcept;
	void SetRemotePrinting(bool isPrinting) noexcept { isRemotePrinting = isPrinting; }
#endif

	static constexpr const char *AllowedAxisLetters = "XYZUVWABCDabcdefghijkl";

	// Standard macro filenames
#define DEPLOYPROBE		"deployprobe"
#define RETRACTPROBE	"retractprobe"
#define FILAMENT_ERROR	"filament-error"
#define TPRE			"tpre"
#define TPOST			"tpost"
#define TFREE			"tfree"
#define deg2rad			0.0174532925

	static constexpr const char* CONFIG_FILE = "config.g";
	static constexpr const char* CONFIG_BACKUP_FILE = "config.g.bak";
	static constexpr const char* BED_EQUATION_G = "bed.g";
	static constexpr const char* MESH_G = "mesh.g";
	static constexpr const char* PAUSE_G = "pause.g";
	static constexpr const char* RESUME_G = "resume.g";
	static constexpr const char* CANCEL_G = "cancel.g";
	static constexpr const char* START_G = "start.g";
	static constexpr const char* STOP_G = "stop.g";
	static constexpr const char* SLEEP_G = "sleep.g";
	static constexpr const char* CONFIG_OVERRIDE_G = "config-override.g";
	static constexpr const char* DefaultHeightMapFile = "heightmap.csv";
	static constexpr const char* LOAD_FILAMENT_G = "load.g";
	static constexpr const char* CONFIG_FILAMENT_G = "config.g";
	static constexpr const char* UNLOAD_FILAMENT_G = "unload.g";
	static constexpr const char* RESUME_AFTER_POWER_FAIL_G = "resurrect.g";
	static constexpr const char* RESUME_PROLOGUE_G = "resurrect-prologue.g";
	static constexpr const char* FILAMENT_CHANGE_G = "filament-change.g";
	static constexpr const char* DAEMON_G = "daemon.g";
	static constexpr const char* RUNONCE_G = "runonce.g";
#if HAS_SMART_DRIVERS
	static constexpr const char* REHOME_G = "rehome.g";
#endif

private:
	GCodes(const GCodes&) = delete;

	enum class HeaterFaultState : uint8_t { noFault, pausePending, timing, stopping, stopped };

	// Resources that can be locked.
	// To avoid deadlock, if you need multiple resources then you must lock them in increasing numerical order.
	typedef uint32_t Resource;
	static const Resource MoveResource = 0;										// Movement system, including canned cycle variables
	static const Resource FileSystemResource = 1;								// Non-sharable parts of the file system
	static const Resource HeaterResourceBase = 2;
	static const size_t NumResources = HeaterResourceBase + 1;

	static_assert(NumResources <= sizeof(Resource) * CHAR_BIT, "Too many resources to keep a bitmap of them in class GCodeMachineState");

	// Codes passed to DoFileMacro
	static constexpr int ToolChangeMacroCode = -1;								// A macro that is being called because of a tool change
	static constexpr int SystemHelperMacroCode = -2;							// Another system macro that is being called to help execute the current command
	static constexpr int AsyncSystemMacroCode = -3;								// A macro that is not being executed as part of a commend being executed, e.g. due to a trigger, filament out etc.

	bool LockResource(const GCodeBuffer& gb, Resource r) noexcept;				// Lock the resource, returning true if success
	bool LockFileSystem(const GCodeBuffer& gb) noexcept;						// Lock the unshareable parts of the file system
	bool LockMovement(const GCodeBuffer& gb) noexcept;							// Lock movement
	void GrabResource(const GCodeBuffer& gb, Resource r) noexcept;				// Grab a resource even if it is already owned
	void GrabMovement(const GCodeBuffer& gb) noexcept;							// Grab the movement lock even if it is already owned
	void UnlockResource(const GCodeBuffer& gb, Resource r) noexcept;			// Unlock the resource if we own it
	void UnlockMovement(const GCodeBuffer& gb) noexcept;						// Unlock the movement resource if we own it

	bool SpinGCodeBuffer(GCodeBuffer& gb) noexcept;								// Do some work on an input channel
	bool StartNextGCode(GCodeBuffer& gb, const StringRef& reply) noexcept;		// Fetch a new or old GCode and process it
	void RunStateMachine(GCodeBuffer& gb, const StringRef& reply) noexcept;		// Execute a step of the state machine
	void DoStraightManualProbe(GCodeBuffer& gb, const StraightProbeSettings& sps);

	void StartPrinting(bool fromStart) noexcept;								// Start printing the file already selected
	void StopPrint(StopPrintReason reason) noexcept;							// Stop the current print

	bool DoFilePrint(GCodeBuffer& gb, const StringRef& reply) noexcept;					// Get G Codes from a file and print them
	bool DoFileMacro(GCodeBuffer& gb, const char* fileName, bool reportMissing, int codeRunning, VariableSet& initialVariables) noexcept;
	bool DoFileMacro(GCodeBuffer& gb, const char* fileName, bool reportMissing, int codeRunning) noexcept;
																						// Run a GCode macro file, optionally report error if not found
	void FileMacroCyclesReturn(GCodeBuffer& gb) noexcept;								// End a macro

	//CNC
	void CloseGodeMultiple() noexcept;
	void CloseModalCommand() noexcept;
	//spindle setting

	//StringParser 'T'
	void ParserT(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	//Print History
	void PrintGcodeHistory(GCodeBuffer& gb) THROWS(GCodeException);

	//M3
	void ModalCmmSpindle(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	//G0/G1
	bool g0_active = false;
	bool g1_active = false;
	void ModalCmmMove(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	//G2/G3
	bool g2_active = false;
	bool g3_active = false;

	//G32
	bool g32_active = false;
	float _X_abs = 0.0 , _Z_abs = 0.0 , _U_inc = 0.0 , _W_inc = 0.0 , _total_Turns = 0.0;
	GCodeResult SetG32(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunG32(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//G76
	bool g76_active = false;
	float _dia_of_thread = 0,_Endpoint_of_thread = 0,_Depth_of_thread = 0,_Depth_of_first_cut = 0,_Pitch = 0;
	float _SZ_76 = 0,_EZ_76 = 0;
	float _Number_of_thread = 0;
	GCodeResult SetG76(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunG76(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//
	//G83
	bool g83_active = false;
	float newX=0, newY=0, _Retractvalue=0, _Depth_of_each_peck=0, _Depth_of_hole=0, _sumQ=0, _real_Z=0;
	int _FeedRate=0;
	GCodeResult SetG83(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunG83(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//G84
	bool g84_active = false;
	float _pitch_of_thread = 0;
	int _spindleRPM = 0;
	GCodeResult SetG84(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunG84(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//M309
	bool CheckHomePosition(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunM1989(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	void RunM1992(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	void RunM1993(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunM1994(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunM1995(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunM1996(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunM1998(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	//M799
	bool RunM799(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	int _timeout;
	//M1990

	//M1998
	bool WaitIOstatus(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	//M1999
	bool GetIOstatus(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

    bool _DoorOpen = true;
    bool _DoorOpen_Pre = true;
    bool _DoorClose = false;
    bool _DoorClose_Pre = false;
    bool _ToolEmpty = true;
    bool _ToolEmpty_Pre = true;
    bool _ToolClamp = true;
    bool _ToolClamp_Pre = true;
    bool _ToolRelease = false;
    bool _ToolRelease_Pre = false;
    bool _ToolDetect = false;
    bool _ToolDetect_Pre = false;

    bool BootToolNumber = true;
    int previousMillToolNumber = -1;
    int nowMillToolNumber = -1;
    float nowToolLength = 0;

	//Helical Boring
	float _A_diaofhole=0,_B_diaofcutter=0;
	int _CW_CCW=0;
	GCodeResult SetM460(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool RunM460(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//circle
	bool RunM2000(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//spiral
	bool RunM2001(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//Gear hobbing
	bool RunM2002(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//Turning Cycle
	bool RunM2003(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//Facing Cycle
	bool RunG72(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//Milling polygon(Side)
	bool RunM2004(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//Milling Side cycle
	bool RunM2005(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//Milling Facing cycle
	bool RunM2006(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//Milling polygon(Facing)
	bool RunM2007(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//Grooving(for lathe)
	bool RunM2008(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//Drilling(for lathe)
	bool RunM2009(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	//

	bool ActOnCode(GCodeBuffer& gb, const StringRef& reply) noexcept;					// Do a G, M or T Code
	bool HandleGcode(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Do a G code
	bool HandleMcode(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Do an M code
	bool HandleTcode(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Do a T code
	bool HandleQcode(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Do an internal code
	bool HandleResult(GCodeBuffer& gb, GCodeResult rslt, const StringRef& reply, OutputBuffer *outBuf)
		pre(outBuf == nullptr || rslt == GCodeResult::ok) noexcept;

	void HandleReply(GCodeBuffer& gb, OutputBuffer *reply) noexcept;
	void HandleReplyPreserveResult(GCodeBuffer& gb, GCodeResult rslt, const char *reply) noexcept;	// Handle G-Code replies

	bool DoStraightMove(GCodeBuffer& gb, bool isCoordinated, const char *& err) SPEED_CRITICAL;	// Execute a straight move
	bool DoArcMove(GCodeBuffer& gb, bool clockwise, const char *& err)				// Execute an arc move
		pre(segmentsLeft == 0; resourceOwners[MoveResource] == &gb);
	void FinaliseMove(GCodeBuffer& gb) noexcept;									// Adjust the move parameters to account for segmentation and/or part of the move having been done already
	bool CheckEnoughAxesHomed(AxesBitmap axesMoved) noexcept;						// Check that enough axes have been homed
	bool TravelToStartPoint(GCodeBuffer& gb) noexcept;								// Set up a move to travel to the resume point

	GCodeResult DoDwell(GCodeBuffer& gb) THROWS(GCodeException);														// Wait for a bit
	GCodeResult DoHome(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);									// Home some axes
	GCodeResult SetOrReportOffsets(GCodeBuffer& gb, const StringRef& reply, int code) THROWS(GCodeException);			// Deal with a G10/M568
	GCodeResult SetPositions(GCodeBuffer& gb) THROWS(GCodeException);													// Deal with a G92
	GCodeResult StraightProbe(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);							// Deal with a G38.x
	GCodeResult DoDriveMapping(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);							// Deal with a M584
	GCodeResult ProbeTool(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);								// Deal with a M585
	GCodeResult FindCenterOfCavity(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);						// Deal with a M675
	GCodeResult SetDateTime(GCodeBuffer& gb,const StringRef& reply) THROWS(GCodeException);								// Deal with a M905
	GCodeResult SavePosition(GCodeBuffer& gb,const StringRef& reply) THROWS(GCodeException);							// Deal with G60
	GCodeResult ConfigureDriver(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);						// Deal with M569
	GCodeResult ConfigureLocalDriver(GCodeBuffer& gb, const StringRef& reply, uint8_t drive) THROWS(GCodeException);	// Deal with M569
#if SUPPORT_ACCELEROMETERS
	GCodeResult ConfigureAccelerometer(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);					// Deal with M955
	GCodeResult StartAccelerometer(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);						// Deal with M956
#endif

	bool SetupM675ProbingMove(GCodeBuffer& gb, bool towardsMin) noexcept;
	void SetupM675BackoffMove(GCodeBuffer& gb, float position) noexcept;
	bool SetupM585ProbingMove(GCodeBuffer& gb) noexcept;
	size_t FindAxisLetter(GCodeBuffer& gb) THROWS(GCodeException);									// Search for and return an axis, throw if none found

	bool ProcessWholeLineComment(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Process a whole-line comment

	const char *LoadExtrusionAndFeedrateFromGCode(GCodeBuffer& gb, bool isPrintingMove);			// Set up the extrusion of a move

	bool Push(GCodeBuffer& gb, bool withinSameFile);								// Push feedrate etc on the stack
	void Pop(GCodeBuffer& gb, bool withinSameFile);									// Pop feedrate etc
	void DisableDrives() noexcept;													// Turn the motors off
	bool SendConfigToLine();														// Deal with M503

	GCodeResult OffsetAxes(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);			// Set/report offsets

#if SUPPORT_WORKPLACE_COORDINATES
	GCodeResult GetSetWorkplaceCoordinates(GCodeBuffer& gb, const StringRef& reply, bool compute) THROWS(GCodeException);	// Set workspace coordinates
# if HAS_MASS_STORAGE
	bool WriteWorkplaceCoordinates(FileStore *f) const noexcept;
# endif
#endif

	GCodeResult ManageTool(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Create a new tool definition
	void SetToolHeaters(Tool *tool, float temperature, bool both) THROWS(GCodeException);	// Set all a tool's heaters to the temperature, for M104/M109
	bool ToolHeatersAtSetTemperatures(const Tool *tool, bool waitWhenCooling, float tolerance) const noexcept;
																				// Wait for the heaters associated with the specified tool to reach their set temperatures
	void GenerateTemperatureReport(const StringRef& reply) const noexcept;		// Store a standard-format temperature report in reply
	OutputBuffer *GenerateJsonStatusResponse(int type, int seq, ResponseSource source) const noexcept;	// Generate a M408 response
	void CheckReportDue(GCodeBuffer& gb, const StringRef& reply) const noexcept;			// Check whether we need to report temperatures or status

	void RestorePosition(const RestorePoint& rp, GCodeBuffer *gb) noexcept;		// Restore user position from a restore point

	void UpdateCurrentUserPosition() noexcept;									// Get the current position from the Move class
	void ToolOffsetTransform(const float coordsIn[MaxAxes], float coordsOut[MaxAxes], AxesBitmap explicitAxes = AxesBitmap()) const noexcept;
																				// Convert user coordinates to head reference point coordinates
	void ToolOffsetInverseTransform(const float coordsIn[MaxAxes], float coordsOut[MaxAxes]) const noexcept;	// Convert head reference point coordinates to user coordinates
	float GetCurrentToolOffset(size_t axis) const noexcept;						// Get an axis offset of the current tool

	GCodeResult RetractFilament(GCodeBuffer& gb, bool retract);					// Retract or un-retract filaments
	GCodeResult LoadFilament(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);		// Load the specified filament into a tool
	GCodeResult UnloadFilament(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);		// Unload the current filament from a tool
	bool ChangeMicrostepping(size_t axisOrExtruder, unsigned int microsteps, bool interp, const StringRef& reply) const noexcept; // Change microstepping on the specified drive
	void CheckTriggers() noexcept;												// Check for and execute triggers
	void CheckFilament() noexcept;												// Check for and respond to filament errors
	void CheckHeaterFault() noexcept;											// Check for and respond to a heater fault, returning true if we should exit
	void DoEmergencyStop() noexcept;											// Execute an emergency stop

	void DoPause(GCodeBuffer& gb, PauseReason reason, const char *msg, uint16_t param = 0) noexcept	// Pause the print
		pre(resourceOwners[movementResource] = &gb);
	void CheckForDeferredPause(GCodeBuffer& gb) noexcept;						// Check if a pause is pending, action it if so

#if HAS_VOLTAGE_MONITOR || HAS_SMART_DRIVERS
	bool DoEmergencyPause() noexcept;											// Do an emergency pause following loss of power or a motor stall
#endif

	bool IsMappedFan(unsigned int fanNumber) noexcept;							// Return true if this fan number is currently being used as a print cooling fan
	void SaveFanSpeeds() noexcept;												// Save the speeds of all fans

	GCodeResult DefineGrid(GCodeBuffer& gb, const StringRef &reply) THROWS(GCodeException);	// Define the probing grid, returning true if error
#if HAS_MASS_STORAGE
	GCodeResult LoadHeightMap(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Load the height map from file
	bool TrySaveHeightMap(const char *filename, const StringRef& reply) const noexcept;	// Save the height map to the specified file
	GCodeResult SaveHeightMap(GCodeBuffer& gb, const StringRef& reply) const;	// Save the height map to the file specified by P parameter
#endif
	void ClearBedMapping();														// Stop using bed compensation
	GCodeResult ProbeGrid(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Start probing the grid, returning true if we didn't because of an error
	ReadLockedPointer<ZProbe> SetZProbeNumber(GCodeBuffer& gb, char probeLetter) THROWS(GCodeException);		// Set up currentZProbeNumber and return the probe
	GCodeResult ExecuteG30(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Probes at a given position - see the comment at the head of the function itself
	void InitialiseTaps(bool fastThenSlow) noexcept;								// Set up to do the first of a possibly multi-tap probe
	void SetBedEquationWithProbe(int sParam, const StringRef& reply);			// Probes a series of points and sets the bed equation

	GCodeResult ConfigureTrigger(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);	// Handle M581
	GCodeResult CheckTrigger(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);		// Handle M582
	GCodeResult UpdateFirmware(GCodeBuffer& gb, const StringRef &reply) THROWS(GCodeException);		// Handle M997
	GCodeResult SendI2c(GCodeBuffer& gb, const StringRef &reply) THROWS(GCodeException);			// Handle M260
	GCodeResult ReceiveI2c(GCodeBuffer& gb, const StringRef &reply) THROWS(GCodeException);			// Handle M261
#if HAS_MASS_STORAGE || HAS_LINUX_INTERFACE
	GCodeResult SimulateFile(GCodeBuffer& gb, const StringRef &reply, const StringRef& file, bool updateFile) THROWS(GCodeException);	// Handle M37 to simulate a whole file
	GCodeResult ChangeSimulationMode(GCodeBuffer& gb, const StringRef &reply, uint32_t newSimulationMode) THROWS(GCodeException);		// Handle M37 to change the simulation mode
#endif
	GCodeResult WaitForPin(GCodeBuffer& gb, const StringRef &reply) THROWS(GCodeException);			// Handle M577

#if HAS_MASS_STORAGE
	GCodeResult WriteConfigOverrideFile(GCodeBuffer& gb, const StringRef& reply) const noexcept; // Write the config-override file
	bool WriteConfigOverrideHeader(FileStore *f) const noexcept;				// Write the config-override header
#endif

	void CheckFinishedRunningConfigFile(GCodeBuffer& gb) noexcept;				// Copy the feed rate etc. from the daemon to the input channels

	MessageType GetMessageBoxDevice(GCodeBuffer& gb) const;						// Decide which device to display a message box on
	void DoManualProbe(GCodeBuffer&, const char *message, const char *title, const AxesBitmap); // Do manual probe in arbitrary direction
	void DoManualBedProbe(GCodeBuffer& gb);										// Do a manual bed probe
	void DeployZProbe(GCodeBuffer& gb) noexcept;
	void RetractZProbe(GCodeBuffer& gb) noexcept;

	void AppendAxes(const StringRef& reply, AxesBitmap axes) const noexcept;	// Append a list of axes to a string

	void EndSimulation(GCodeBuffer *gb) noexcept;								// Restore positions etc. when exiting simulation mode
	bool IsCodeQueueIdle() const noexcept;										// Return true if the code queue is idle

#if HAS_MASS_STORAGE
	void SaveResumeInfo(bool wasPowerFailure) noexcept;
#endif

	void NewMoveAvailable(unsigned int sl) noexcept;							// Flag that a new move is available
	void NewMoveAvailable() noexcept;											// Flag that a new move is available

	void SetMoveBufferDefaults() noexcept;										// Set up default values in the move buffer
	void ChangeExtrusionFactor(unsigned int extruder, float factor) noexcept;	// Change a live extrusion factor

#if SUPPORT_12864_LCD
	int GetHeaterNumber(unsigned int itemNumber) const noexcept;
#endif
	Pwm_t ConvertLaserPwm(float reqVal) const noexcept;

#if HAS_AUX_DEVICES
#if !ALLOW_ARBITRARY_PANELDUE_PORT
	static constexpr
#endif
	uint8_t serialChannelForPanelDueFlashing
#if !ALLOW_ARBITRARY_PANELDUE_PORT
	= 1
#endif
	;
	static bool emergencyStopCommanded;
	static void CommandEmergencyStop(UARTClass *p) noexcept;
#endif

	Platform& platform;													// The RepRap machine

#if HAS_NETWORKING || HAS_LINUX_INTERFACE
	NetworkGCodeInput* httpInput;										// These cache incoming G-codes...
	NetworkGCodeInput* telnetInput;										// ...
#endif

	GCodeBuffer* gcodeSources[NumGCodeChannels];						// The various sources of gcodes

	GCodeBuffer*& httpGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::HTTP)];
	GCodeBuffer*& telnetGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::Telnet)];
	GCodeBuffer*& fileGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::File)];
	GCodeBuffer*& usbGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::USB)];
	GCodeBuffer*& auxGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::Aux)];					// This one is for the PanelDue on the async serial interface
	GCodeBuffer*& triggerGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::Trigger)];			// Used for executing config.g and trigger macro files
	GCodeBuffer*& queuedGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::Queue)];
	GCodeBuffer*& lcdGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::LCD)];					// This one for the 12864 LCD
	GCodeBuffer*& spiGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::SBC)];
	GCodeBuffer*& daemonGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::Daemon)];
	GCodeBuffer*& aux2GCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::Aux2)];				// This one is reserved for the second async serial interface
	GCodeBuffer*& autoPauseGCode = gcodeSources[GCodeChannel::ToBaseType(GCodeChannel::Autopause)];		// ***THIS ONE MUST BE LAST*** GCode state machine used to run macros on power fail, heater faults and filament out

	size_t nextGcodeSource;												// The one to check next, using round-robin scheduling

	static Mutex resourceMutex;
	const GCodeBuffer* resourceOwners[NumResources];					// Which gcode buffer owns each resource

	StraightProbeSettings straightProbeSettings;						// G38 straight probe settings
	union
	{
		M675Settings m675Settings;
		M585Settings m585Settings;
	};

	MachineType machineType;					// whether FFF, laser or CNC
	bool active;								// Live and running?
#if HAS_LINUX_INTERFACE
	FilePosition lastFilePosition;				// Last known file position
#endif
	const char *deferredPauseCommandPending;
	PauseState pauseState;						// whether the machine is running normally or is pausing, paused or resuming
	bool runningConfigFile;						// We are running config.g during the startup process
	bool doingToolChange;						// We are running tool change macros

#if HAS_VOLTAGE_MONITOR
	bool isPowerFailPaused;						// true if the print was paused automatically because of a power failure
	char *powerFailScript;						// the commands run when there is a power failure
#endif

	// The current user position now holds the requested user position after applying workplace coordinate offsets.
	// So we must subtract the workplace coordinate offsets when we want to display them.
	// We have chosen this approach because it allows us to switch workplace coordinates systems or turn off applying workplace offsets without having to update currentUserPosition.
	float currentUserPosition[MaxAxes];			// The current position of the axes as commanded by the input gcode, after accounting for workplace offset, before accounting for tool offset and Z hop
	float currentZHop;							// The amount of Z hop that is currently applied

	// The following contain the details of moves that the Move module fetches
	// CAUTION: segmentsLeft should ONLY be changed from 0 to not 0 by calling NewMoveAvailable()!
	ExtendedRawMove moveBuffer;					// Move details
	bool updateUserPosition;

	unsigned int segmentsLeftToStartAt;
	float moveFractionToSkip;
	float firstSegmentFractionToSkip;

	float restartMoveFractionDone;				// how much of the next move was printed before the pause or power failure (from M26)
	float restartInitialUserC0;					// if the print was paused during an arc move, the user X coordinate at the start of that move (from M26)
	float restartInitialUserC1;					// if the print was paused during an arc move, the user Y coordinate at the start of that move (from M26)
	float ServoStatus[MaxAxes] = {0.0};
    int MPG_MODE = 0; //0:OFF 1:ON

	RestorePoint simulationRestorePoint;		// The position and feed rate when we started a simulation

	RestorePoint numberedRestorePoints[NumRestorePoints];				// Restore points accessible using the R parameter in the G0/G1 command
	RestorePoint& pauseRestorePoint = numberedRestorePoints[1];			// The position and feed rate when we paused the print
	RestorePoint& toolChangeRestorePoint = numberedRestorePoints[2];	// The position and feed rate when we freed a tool

	size_t numTotalAxes;						// How many axes we have
	size_t numVisibleAxes;						// How many axes are visible
	size_t numExtruders;						// How many extruders we have, or may have
	float axisScaleFactors[MaxAxes];			// Scale XYZ coordinates by this factor
	float virtualExtruderPosition;				// Virtual extruder position of the last move fed into the Move class
	float rawExtruderTotalByDrive[MaxExtruders]; // Extrusion amount in the last G1 command with an E parameter when in absolute extrusion mode
	float rawExtruderTotal;						// Total extrusion amount fed to Move class since starting print, before applying extrusion factor, summed over all drives

	unsigned int currentCoordinateSystem;		// This is zero-based, where as the P parameter in the G10 command is 1-based
	float workplaceCoordinates[NumCoordinateSystems][MaxAxes];	// Workplace coordinate offsets

#if HAS_MASS_STORAGE
	FileData fileToPrint;						// The next file to print
	FilePosition fileOffsetToPrint;				// The offset to print from
#endif

	// Tool change. These variables can be global because movement is locked while doing a tool change, so only one can take place at a time.
	int16_t newToolNumber;
	uint8_t toolChangeParam;

	char axisLetters[MaxAxes + 1];				// The names of the axes, with a null terminator
	bool limitAxes;								// Don't think outside the box
	bool noMovesBeforeHoming;					// Don't allow movement prior to homing the associates axes

	AxesBitmap toBeHomed;						// Bitmap of axes still to be homed
	AxesBitmap axesHomed;						// Bitmap of which axes have been homed
	AxesBitmap axesVirtuallyHomed;				// same as axesHomed except all bits are set when simulating

	float pausedFanSpeeds[MaxFans];				// Fan speeds when the print was paused or a tool change started
	float lastDefaultFanSpeed;					// Last speed given in a M106 command with no fan number
	float speedFactor;							// speed factor as a fraction (normally 1.0)
	float extrusionFactors[MaxExtruders];		// extrusion factors (normally 1.0)
	float volumetricExtrusionFactors[MaxExtruders]; // Volumetric extrusion factors
	float currentBabyStepOffsets[MaxAxes];		// The accumulated axis offsets due to baby stepping requests

	// Z probe
	GridDefinition defaultGrid;					// The grid defined by the M557 command in config.g
	int32_t g30ProbePointIndex;					// the index of the point we are probing (G30 P parameter), or -1 if none
	int g30SValue;								// S parameter in the G30 command, or -2 if there wasn't one
	float g30HValue;							// H parameter in the G30 command, or 0.0 if there wasn't on
	float g30zHeightError;						// the height error last time we probed
	float g30PrevHeightError;					// the height error the previous time we probed
	float g30zHeightErrorSum;					// the sum of the height errors for the current probe point
	float g30zHeightErrorLowestDiff;			// the lowest difference we have seen between consecutive readings
	uint32_t lastProbedTime;					// time in milliseconds that the probe was last triggered
	volatile bool zProbeTriggered;				// Set by the step ISR when a move is aborted because the Z probe is triggered
	size_t gridAxis0index, gridAxis1index;		// Which grid probe point is next
	bool doingManualBedProbe;					// true if we are waiting for the user to jog the nozzle until it touches the bed
	bool hadProbingError;						// true if there was an error probing the last point
	bool zDatumSetByProbing;					// true if the Z position was last set by probing, not by an endstop switch or by G92
	int8_t tapsDone;							// how many times we tapped the current point
	uint8_t currentZProbeNumber;				// which Z probe a G29 or G30 command is using

	// Simulation and print time
	float simulationTime;						// Accumulated simulation time
	uint32_t lastDuration;						// Time or simulated time of the last successful print or simulation, in seconds
	uint8_t simulationMode;						// 0 = not simulating, 1 = simulating, >1 are simulation modes for debugging
	bool exitSimulationWhenFileComplete;		// true if simulating a file
	bool updateFileWhenSimulationComplete;		// true if simulated time should be appended to the file

	// Triggers
	Trigger triggers[MaxTriggers];				// Trigger conditions
	TriggerNumbersBitmap triggersPending;		// Bitmap of triggers pending but not yet executed

	// Firmware update
	Bitmap<uint8_t> firmwareUpdateModuleMap;	// Bitmap of firmware modules to be updated
	bool isFlashing;							// Is a new firmware binary going to be flashed?
	bool isFlashingPanelDue;					// Are we in the process of flashing PanelDue?

	// Code queue
	GCodeQueue *codeQueue;						// Stores certain codes for deferred execution


#if HAS_MASS_STORAGE
	// SHA1 hashing
	FileStore *fileBeingHashed;
	SHA1Context hash;
	bool StartHash(const char* filename) noexcept;
	GCodeResult AdvanceHash(const StringRef &reply) noexcept;
#endif

	// Filament monitoring
	FilamentSensorStatus lastFilamentError;
	size_t lastFilamentErrorExtruder;

	// Laser
	float laserMaxPower;
	bool laserPowerSticky;						// true if G1 S parameters are remembered across G1 commands

	// Heater fault handler
	HeaterFaultState heaterFaultState;			// whether there is a heater fault and what we have done about it so far
	uint32_t heaterFaultTime;					// when the heater fault occurred
	uint32_t heaterFaultTimeout;				// how long we wait for the user to fix it before turning everything off

	// Object cancellation
	ObjectTracker buildObjects;

	// Misc
	uint32_t lastWarningMillis;					// When we last sent a warning message for things that can happen very often
	AxesBitmap axesToSenseLength;				// The axes on which we are performing axis length sensing
	bool atxPowerControlled;

#if HAS_MASS_STORAGE
	static constexpr uint32_t SdTimingByteIncrement = 8 * 1024;	// how many timing bytes we write at a time
	static constexpr const char *TimingFileName = "test.tst";	// the name of the file we write
	FileStore *sdTimingFile;					// file handle being used for SD card write timing
	uint32_t timingBytesRequested;				// how many bytes we were asked to write
	uint32_t timingBytesWritten;				// how many timing bytes we have written so far
	uint32_t timingStartMillis;
#endif

#if SUPPORT_REMOTE_COMMANDS
	bool isRemotePrinting;
#endif

	int8_t lastAuxStatusReportType;				// The type of the last status report requested by PanelDue
	bool isWaiting;								// True if waiting to reach temperature
	bool cancelWait;							// Set true to cancel waiting
	bool displayNoToolWarning;					// True if we need to display a 'no tool selected' warning
	bool m501SeenInConfigFile;					// true if M501 was executed form config.g
	bool daemonRunning;
	char filamentToLoad[FilamentNameLength];	// Name of the filament being loaded

	static constexpr const float MinServoPulseWidth = 544.0, MaxServoPulseWidth = 2400.0;

	static constexpr int8_t ObjectModelAuxStatusReportType = 100;		// A non-negative value distinct from any M408 report type
};

// Get the total baby stepping offset for an axis
inline float GCodes::GetTotalBabyStepOffset(size_t axis) const noexcept
{
	return currentBabyStepOffsets[axis];
}

// Get the axis status
inline float GCodes::GetServoStatus(size_t axis) const noexcept
{
	return ServoStatus[axis];
}

//*****************************************************************************************************

#endif

#include "small_animations.h"

#include "water_geometry.h"
#include "max7219.h"
#include "mpu6050.h"
#include "display/icons.h"

#include <Arduino.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>

namespace SmallAnimations {
namespace {

// ============================================================
// CONFIGURATION
// ============================================================

constexpr uint8_t MATRIX_MODULE_SIZE = 8;

constexpr uint16_t NODE_COUNT =
    WaterGeometry::NODE_COUNT;

// ------------------------------------------------------------
// HEART ANIMATION
// ------------------------------------------------------------

constexpr uint32_t HEART_UPDATE_MS = 45;

constexpr float HEART_PULSE_PERIOD_MS = 900.0f;

uint8_t heartBaseBrightness = 5;

// ------------------------------------------------------------
// RAIN ANIMATION
// ------------------------------------------------------------

constexpr uint8_t MAX_RAIN_DROPS = 10;

constexpr uint32_t RAIN_SPAWN_INTERVAL_MS = 230;
constexpr uint32_t RAIN_MOVE_INTERVAL_MS = 100;

constexpr float RAIN_UPSTREAM_BAND = 1.4f;

constexpr uint32_t RAIN_MAX_LIFETIME_MS = 10000;

// ------------------------------------------------------------
// WAVE ANIMATION
// ------------------------------------------------------------

constexpr uint32_t WAVE_UPDATE_MS = 45;

constexpr float WAVE_FREQUENCY = 1.65f;
constexpr float WAVE_SPEED = 0.006f;

constexpr float WAVE_THRESHOLD = 0.25f;

// ------------------------------------------------------------
// MPU6050 GRAVITY ESTIMATION
// ------------------------------------------------------------

// These signs follow the known EMOTIX360 MPU mounting.
constexpr float FLOW_X_SIGN = 1.0f;
constexpr float FLOW_Y_SIGN = 1.0f;

constexpr float GYRO_X_SIGN = 1.0f;

constexpr float ACCEL_CORRECTION_TAU = 0.25f;
constexpr float GRAVITY_MAGNITUDE_TAU = 0.12f;

constexpr float RADIANS_PER_DEGREE =
    0.017453292519943295f;

// ============================================================
// INTERNAL DATA STRUCTURES
// ============================================================

struct BorderNode
{
    uint8_t nodeID;
    float angle;
};

// Used to build a centre-outward startup fill sequence.
struct FillNode
{
    uint8_t nodeID;

    // Distance from the physical centre line of the assembly.
    float centerDistance;

    // Secondary ordering for LEDs at similar centre distances.
    float sideDistance;
};

struct RainDrop
{
    uint8_t nodeID;
    uint8_t previousNodeID;

    bool hasPrevious;
    bool active;

    uint32_t lastMoveMs;
    uint32_t bornMs;
};

// ============================================================
// INTERNAL STATE
// ============================================================

bool initialized = false;

Effect currentEffect = Effect::STARTUP;

uint32_t effectStartedMs = 0;
uint32_t lastEffectRenderMs = 0;

// ------------------------------------------------------------
// PHYSICAL GEOMETRY BOUNDS
// ------------------------------------------------------------

float geometryMinX = 0.0f;
float geometryMaxX = 0.0f;

float geometryMinY = 0.0f;
float geometryMaxY = 0.0f;

float geometryCenterX = 0.0f;
float geometryCenterY = 0.0f;

float geometryHalfWidth = 1.0f;
float geometryHalfHeight = 1.0f;

// ------------------------------------------------------------
// STARTUP ANIMATION
// ------------------------------------------------------------

constexpr uint32_t STARTUP_STEP_MS = 22;

// Pause after the perimeter has been completed.
constexpr uint32_t STARTUP_HOLD_MS = 600;

// Centre-outward interior fill speed.
constexpr uint32_t STARTUP_FILL_STEP_MS = 30;

// Full-display breathing pulse.
constexpr uint32_t STARTUP_PULSE_PERIOD_MS = 1500;
constexpr uint32_t STARTUP_PULSE_DURATION_MS = 4500;

constexpr float STARTUP_PULSE_MIN_FACTOR = 0.30f;

// ------------------------------------------------------------
// STARTUP -> HEART TRANSITION
// ------------------------------------------------------------

// Time taken to transform the full display into two heart icons.
constexpr uint32_t STARTUP_TRANSITION_DURATION_MS = 1800;

// Final brightness curve must match updateHeart().
constexpr float HEART_PULSE_MIN_FACTOR = 0.55f;

// Smooth transition state.
uint8_t startupHeartMask[NODE_COUNT] = {};
uint8_t startupTransitionOffOrder[NODE_COUNT] = {};

uint16_t startupTransitionOffCount = 0;
uint16_t lastTransitionOffShown = UINT16_MAX;

bool startupHandoffPending = false;

// ------------------------------------------------------------
// STARTUP ANIMATION STATE
// ------------------------------------------------------------

BorderNode borderOrder[NODE_COUNT] = {};

uint16_t borderCount = 0;
uint16_t lastBorderShown = 0xFFFFU;

// Interior LEDs, sorted from the hourglass neck outward.
FillNode startupFillOrder[NODE_COUNT] = {};

uint16_t startupFillCount = 0;
uint16_t lastFillShown = 0xFFFFU;

// Startup completion status.
bool startupComplete = false;

// Preserve the user's normal MAX7219 brightness.
uint8_t startupBaseBrightness = 5;

uint8_t lastStartupBrightness = 0xFFU;
bool startupBrightnessCaptured = false;

// ------------------------------------------------------------
// GRAVITY STATE
// ------------------------------------------------------------

/*
 * Gravity angle:
 *
 *     0       = screen down.
 *     +PI/2   = screen right.
 *     -PI/2   = screen left.
 *     PI      = screen up.
 */

float gravityAngleRad = 0.0f;

float gravityMagnitude = 1.0f;

float gravityX = 0.0f;
float gravityY = 1.0f;

bool gravityInitialized = false;

bool imuTimestampInitialized = false;

uint32_t lastImuTimestampMs = 0;

// ------------------------------------------------------------
// RAIN DROPS
// ------------------------------------------------------------

RainDrop rainDrops[MAX_RAIN_DROPS] = {};

uint32_t lastRainSpawnMs = 0;
uint32_t lastRainUpdateMs = 0;

// ------------------------------------------------------------
// WAVE STATE
// ------------------------------------------------------------

float wavePhase = 0.0f;

// ============================================================
// FUNCTION FORWARD DECLARATIONS
// ============================================================

void setEffectInternal(Effect effect, uint32_t nowMs);

// Startup-to-heart transition is defined after updateStartup().
void updateStartupHeartTransition(uint32_t transitionElapsedMs);

// ============================================================
// GENERAL HELPERS
// ============================================================

float clampFloat(
    float value,
    float minimum,
    float maximum
)
{
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

float normalizeAngle(float angle)
{
    return atan2f(sinf(angle), cosf(angle));
}

// ============================================================
// PHYSICAL NODE HELPERS
// ============================================================

/*
 * GET NODE POSITION
 *
 * WaterGeometry already accounts for:
 *
 *     - Upper slave -45 degrees.
 *     - Lower master +45 degrees.
 *     - Module centre separation.
 *
 * All effects use these common physical coordinates.
 */
const WaterGeometry::Node* getPhysicalNode(uint8_t nodeID)
{
    return WaterGeometry::getNode(nodeID);
}

/*
 * DRAW ONE PHYSICAL NODE
 *
 * Node ID is converted using the existing logical MAX7219
 * framebuffer mapping.
 */
void setNode(uint8_t nodeID, uint8_t state)
{
    uint8_t x;
    uint8_t y;

    if (!WaterGeometry::getCoordinates(nodeID, x, y)) {
        return;
    }

    MAX7219_SetPixel(x, y, state);
}

/*
 * RENDER PHYSICAL NODE FRAME
 *
 * The input array uses physical geometry node IDs.
 *
 * The actual MAX7219 framebuffer mapping remains unchanged.
 */
void renderNodeFrame(const bool frame[NODE_COUNT])
{
    MAX7219_Clear();

    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        if (frame[id]) {
            setNode((uint8_t)id, 1);
        }
    }

    MAX7219_Update();
}

// ============================================================
// PHYSICAL GEOMETRY INITIALIZATION
// ============================================================

/*
 * CALCULATE GEOMETRY BOUNDS
 *
 * Determine the complete physical LED arrangement bounds.
 *
 * These coordinates are used by the heart animation.
 */
bool calculateGeometryBounds()
{
    geometryMinX = 100000.0f;
    geometryMaxX = -100000.0f;

    geometryMinY = 100000.0f;
    geometryMaxY = -100000.0f;

    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        const WaterGeometry::Node *node =
            getPhysicalNode((uint8_t)id);

        if (node == nullptr) {
            return false;
        }

        geometryMinX =
            fminf(geometryMinX, node->x);

        geometryMaxX =
            fmaxf(geometryMaxX, node->x);

        geometryMinY =
            fminf(geometryMinY, node->y);

        geometryMaxY =
            fmaxf(geometryMaxY, node->y);
    }

    geometryCenterX =
        (geometryMinX + geometryMaxX) * 0.5f;

    geometryCenterY =
        (geometryMinY + geometryMaxY) * 0.5f;

    geometryHalfWidth =
        (geometryMaxX - geometryMinX) * 0.5f;

    geometryHalfHeight =
        (geometryMaxY - geometryMinY) * 0.5f;

    if (geometryHalfWidth < 0.001f ||
        geometryHalfHeight < 0.001f)
    {
        return false;
    }

    return true;
}

// ============================================================
// STARTUP BORDER ANIMATION
// ============================================================

/*
 * BUILD STARTUP BORDER ORDER
 *
 * Identify the original eight-by-eight perimeter LEDs
 * on both physical modules.
 *
 * Use each node's physical coordinate to sort the
 * border animation around the complete assembly.
 */
bool buildBorderOrder()
{
    borderCount = 0;

    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        uint8_t x;
        uint8_t y;

        if (!WaterGeometry::getCoordinates(
                (uint8_t)id,
                x,
                y))
        {
            return false;
        }

        // Convert logical framebuffer X into module-local X.
        const uint8_t localX =
            (uint8_t)(x % MATRIX_MODULE_SIZE);

        // Every individual module has its own perimeter.
        const bool isBorder =
            localX == 0 ||
            localX == MATRIX_MODULE_SIZE - 1 ||
            y == 0 ||
            y == MATRIX_MODULE_SIZE - 1;

        if (!isBorder) {
            continue;
        }

        if (borderCount >= NODE_COUNT) {
            return false;
        }

        const WaterGeometry::Node *node =
            getPhysicalNode((uint8_t)id);

        if (node == nullptr) {
            return false;
        }

        // Calculate angular position around the complete assembly.
        const float angle =
            atan2f(
                node->y - geometryCenterY,
                node->x - geometryCenterX
            );

        borderOrder[borderCount].nodeID =
            (uint8_t)id;

        borderOrder[borderCount].angle = angle;

        ++borderCount;
    }

    // Sort border nodes by physical angle.
    // Insertion sort is sufficient for this small array.
    for (uint8_t i = 1; i < borderCount; ++i)
    {
        const BorderNode key = borderOrder[i];

        int16_t j = (int16_t)i - 1;

        while (j >= 0 &&
               borderOrder[j].angle > key.angle)
        {
            borderOrder[j + 1] = borderOrder[j];

            --j;
        }

        borderOrder[j + 1] = key;
    }

    return borderCount > 0;
}

/*
 * BUILD STARTUP FILL ORDER
 *
 * The border is illuminated first.
 *
 * Then illuminate the interior LEDs beginning near the
 * physical centre line and progressing toward both ends.
 *
 * Physical coordinates come from WaterGeometry, so this
 * pattern follows the assembled hourglass geometry rather
 * than assuming an ordinary rectangular 16x8 display.
 */
bool buildStartupFillOrder()
{
    startupFillCount = 0;

    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        uint8_t x;
        uint8_t y;

        if (!WaterGeometry::getCoordinates(
                (uint8_t)id,
                x,
                y))
        {
            return false;
        }

        const uint8_t localX =
            (uint8_t)(x % MATRIX_MODULE_SIZE);

        const bool isBorder =
            localX == 0 ||
            localX == MATRIX_MODULE_SIZE - 1 ||
            y == 0 ||
            y == MATRIX_MODULE_SIZE - 1;

        // The border is handled by buildBorderOrder().
        if (isBorder) {
            continue;
        }

        if (startupFillCount >= NODE_COUNT) {
            return false;
        }

        const WaterGeometry::Node *node =
            getPhysicalNode((uint8_t)id);

        if (node == nullptr) {
            return false;
        }

        FillNode &entry =
            startupFillOrder[startupFillCount];

        entry.nodeID = (uint8_t)id;

        // First priority: approach the centre line.
        entry.centerDistance =
            fabsf(node->y - geometryCenterY);

        // Second priority: approach the horizontal centre.
        entry.sideDistance =
            fabsf(node->x - geometryCenterX);

        ++startupFillCount;
    }

    // Sort from the centre line outward.
    // Insertion sort is sufficient for 128 or fewer nodes.
    for (uint16_t i = 1; i < startupFillCount; ++i)
    {
        const FillNode key = startupFillOrder[i];

        int16_t j = (int16_t)i - 1;

        while (j >= 0)
        {
            const FillNode &previous =
                startupFillOrder[j];

            const bool sameCentreDistance =
                fabsf(
                    previous.centerDistance -
                    key.centerDistance
                ) < 0.0001f;

            const bool shouldMove =
                previous.centerDistance > key.centerDistance ||
                (
                    sameCentreDistance &&
                    previous.sideDistance > key.sideDistance
                );

            if (!shouldMove) {
                break;
            }

            startupFillOrder[j + 1] =
                startupFillOrder[j];

            --j;
        }

        startupFillOrder[j + 1] = key;
    }

    return startupFillCount > 0;
}

/*
 * BUILD STARTUP -> HEART TRANSITION ORDER
 *
 * Create a mask containing the exact pixels used by the
 * double-heart icon.
 *
 * Then create an order for turning off every other LED.
 *
 * Removal order:
 *     1. Interior pixels, in reverse startup-fill order.
 *     2. Border pixels, in reverse border-sweep order.
 *
 * Heart pixels are never included in the removal order.
 *
 * This allows the full display to gradually become the
 * two original 8x8 heart icons without changing their shape.
 */
bool buildStartupTransitionOrder()
{
    if (Icon_Heart.data == nullptr ||
        Icon_Heart.width != 8 ||
        Icon_Heart.height != 8)
    {
        Serial.println(
            "[SMALL ANIMATIONS] Invalid heart icon for transition"
        );

        return false;
    }

    memset(startupHeartMask, 0, sizeof(startupHeartMask));

    startupTransitionOffCount = 0;

    bool alreadyAdded[NODE_COUNT] = {};

    uint16_t heartPixelCount = 0;

    // --------------------------------------------------------
    // BUILD THE DOUBLE-HEART MASK
    // --------------------------------------------------------

    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        uint8_t x;
        uint8_t y;

        if (!WaterGeometry::getCoordinates(
                (uint8_t)id,
                x,
                y))
        {
            return false;
        }

        // Both physical modules use the same 8x8 icon.
        const uint8_t localX =
            (uint8_t)(x % MATRIX_MODULE_SIZE);

        const uint8_t row =
            Icon_Heart.data[y];

        const bool isHeartPixel =
            (row & (uint8_t)(0x80U >> localX)) != 0;

        if (isHeartPixel)
        {
            startupHeartMask[id] = 1;
            ++heartPixelCount;
        }
    }

    // --------------------------------------------------------
    // ADD INTERIOR BACKGROUND PIXELS
    // --------------------------------------------------------

    // Reverse the original centre-outward fill sequence.
    for (int16_t i = (int16_t)startupFillCount - 1;
         i >= 0;
         --i)
    {
        const uint8_t id =
            startupFillOrder[i].nodeID;

        // Never remove a heart pixel.
        if (startupHeartMask[id]) {
            continue;
        }

        if (alreadyAdded[id]) {
            continue;
        }

        if (startupTransitionOffCount >= NODE_COUNT) {
            return false;
        }

        startupTransitionOffOrder[
            startupTransitionOffCount++
        ] = id;

        alreadyAdded[id] = true;
    }

    // --------------------------------------------------------
    // ADD BORDER BACKGROUND PIXELS
    // --------------------------------------------------------

    // Reverse the original perimeter sweep sequence.
    for (int16_t i = (int16_t)borderCount - 1;
         i >= 0;
         --i)
    {
        const uint8_t id =
            borderOrder[i].nodeID;

        // Some heart pixels also belong to the module border.
        if (startupHeartMask[id]) {
            continue;
        }

        if (alreadyAdded[id]) {
            continue;
        }

        if (startupTransitionOffCount >= NODE_COUNT) {
            return false;
        }

        startupTransitionOffOrder[
            startupTransitionOffCount++
        ] = id;

        alreadyAdded[id] = true;
    }

    // --------------------------------------------------------
    // SAFETY CHECK
    // --------------------------------------------------------

    // Include any remaining background pixels in case the
    // border or fill order does not cover a particular node.
    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        if (startupHeartMask[id] || alreadyAdded[id]) {
            continue;
        }

        if (startupTransitionOffCount >= NODE_COUNT) {
            return false;
        }

        startupTransitionOffOrder[
            startupTransitionOffCount++
        ] = (uint8_t)id;

        alreadyAdded[id] = true;
    }

    // The icon contains 32 pixels per module, 64 in total.
    // All remaining LEDs must be included in the removal list.
    if (heartPixelCount != 64 ||
        startupTransitionOffCount !=
            (NODE_COUNT - heartPixelCount))
    {
        Serial.println(
            "[SMALL ANIMATIONS] Heart transition order validation failed"
        );

        return false;
    }

    Serial.printf(
        "[SMALL ANIMATIONS] Heart transition ready: "
        "%u heart pixels, %u background pixels\n",
        heartPixelCount,
        startupTransitionOffCount
    );

    return true;
}

/*
 * UPDATE STARTUP ANIMATION
 *
 * STAGE 1:
 *     Illuminate the physical perimeter LEDs one at a time.
 *
 * STAGE 2:
 *     Hold the completed perimeter briefly.
 *
 * STAGE 3:
 *     Fill the interior from the hourglass centre outward.
 *
*
 * STAGE 4:
 *     Pulse the fully illuminated display three times.
 *
 * STAGE 5:
 *     Gradually remove background pixels and transition
 *     into the double-heart animation.
 *
 * When all stages finish, set startupComplete.
 *
 * AnimationManager handles the transition to HEART.
 */
void updateStartup(uint32_t nowMs)
{
    if (startupComplete) {
        return;
    }

    const uint32_t elapsedMs =
        (uint32_t)(nowMs - effectStartedMs);

    // --------------------------------------------------------
    // CALCULATE STARTUP STAGE DURATIONS
    // --------------------------------------------------------

    const uint32_t borderDuration =
        (uint32_t)borderCount * STARTUP_STEP_MS;

    const uint32_t fillStartTime =
        borderDuration + STARTUP_HOLD_MS;

    const uint32_t fillDuration =
        (uint32_t)startupFillCount * STARTUP_FILL_STEP_MS;

    const uint32_t pulseStartTime =
        fillStartTime + fillDuration;


    // --------------------------------------------------------
    // STAGE 1: BORDER SWEEP
    // --------------------------------------------------------

    uint16_t requestedBorder =
        (uint16_t)(elapsedMs / STARTUP_STEP_MS);

    if (requestedBorder > borderCount) {
        requestedBorder = borderCount;
    }


    // --------------------------------------------------------
    // STAGE 2 / 3: INTERIOR FILL
    // --------------------------------------------------------

    uint16_t requestedFill = 0;

    if (elapsedMs >= fillStartTime)
    {
        requestedFill =
            (uint16_t)(
                (elapsedMs - fillStartTime) /
                STARTUP_FILL_STEP_MS
            );

        if (requestedFill > startupFillCount) {
            requestedFill = startupFillCount;
        }
    }


    // --------------------------------------------------------
    // RENDER ONLY WHEN THE PATTERN CHANGES
    // --------------------------------------------------------

    if (requestedBorder != lastBorderShown ||
        requestedFill != lastFillShown)
    {
        bool frame[NODE_COUNT] = {};

        // Draw completed border LEDs.
        for (uint16_t i = 0; i < requestedBorder; ++i)
        {
            frame[borderOrder[i].nodeID] = true;
        }

        // Draw completed interior-fill LEDs.
        for (uint16_t i = 0; i < requestedFill; ++i)
        {
            frame[startupFillOrder[i].nodeID] = true;
        }

        renderNodeFrame(frame);

        lastBorderShown = requestedBorder;
        lastFillShown = requestedFill;
    }


    // --------------------------------------------------------
    // STAGE 4: FULL-DISPLAY BREATHING PULSE
    // --------------------------------------------------------

    if (elapsedMs < pulseStartTime) {
        return;
    }

    const uint32_t pulseElapsedMs =
        elapsedMs - pulseStartTime;


    // --------------------------------------------------------
    // STAGE 4A: NORMAL STARTUP BREATHING
    // --------------------------------------------------------

    if (pulseElapsedMs < STARTUP_PULSE_DURATION_MS)
    {
        const float phase =
            2.0f * 3.14159265358979323846f *
            (
                (float)(pulseElapsedMs % STARTUP_PULSE_PERIOD_MS) /
                (float)STARTUP_PULSE_PERIOD_MS
            );

        const float pulse =
            0.5f - (0.5f * cosf(phase));

        const float brightnessFactor =
            STARTUP_PULSE_MIN_FACTOR +
            (
                (1.0f - STARTUP_PULSE_MIN_FACTOR) *
                pulse
            );

        uint8_t brightness =
            (uint8_t)lroundf(
                startupBaseBrightness * brightnessFactor
            );

        // Preserve the original OFF state.
        if (startupBaseBrightness > 0 && brightness == 0) {
            brightness = 1;
        }

        if (brightness != lastStartupBrightness)
        {
            MAX7219_SetBrightness(brightness);
            lastStartupBrightness = brightness;
        }

        return;
    }


    // --------------------------------------------------------
    // STAGE 5: SMOOTH TRANSITION FROM FULL DISPLAY TO HEART
    // --------------------------------------------------------

    const uint32_t transitionElapsedMs =
        pulseElapsedMs - STARTUP_PULSE_DURATION_MS;

    updateStartupHeartTransition(transitionElapsedMs);
    
}

/*
 * UPDATE STARTUP -> HEART TRANSITION
 *
 * STAGE 5:
 *
 * Gradually transform the fully illuminated display into
 * the double-heart pattern.
 *
 * PIXEL TRANSITION:
 *     - Heart pixels remain illuminated.
 *     - Background pixels are turned off progressively.
 *     - The original heart bitmap is never modified.
 *
 * BRIGHTNESS TRANSITION:
 *     - Starts with the startup breathing brightness.
 *     - The minimum brightness changes from 30% to 55%.
 *     - The pulse period changes from 1500ms to 900ms.
 *     - The final brightness matches updateHeart() at phase zero.
 *
 * Smoothstep interpolation prevents a sudden change in the
 * pulse speed or brightness at either end of this transition.
 */
void updateStartupHeartTransition(uint32_t transitionElapsedMs)
{
    // --------------------------------------------------------
    // CALCULATE TRANSITION PROGRESS
    // --------------------------------------------------------

    float progress =
        (float)transitionElapsedMs /
        (float)STARTUP_TRANSITION_DURATION_MS;

    if (progress > 1.0f) {
        progress = 1.0f;
    }

    // Smoothstep: 0 -> 1 with zero slope at both ends.
    const float easedProgress =
        progress * progress *
        (3.0f - (2.0f * progress));


    // --------------------------------------------------------
    // STAGE 5A: REMOVE BACKGROUND PIXELS
    // --------------------------------------------------------

    uint16_t requestedOff =
        (uint16_t)(
            (float)startupTransitionOffCount *
            easedProgress
        );

    if (requestedOff > startupTransitionOffCount) {
        requestedOff = startupTransitionOffCount;
    }

    // Render only when the number of removed pixels changes.
    if (requestedOff != lastTransitionOffShown)
    {
        bool frame[NODE_COUNT] = {};

        // Keep every heart pixel illuminated.
        for (uint16_t id = 0; id < NODE_COUNT; ++id)
        {
            if (startupHeartMask[id]) {
                frame[id] = true;
            }
        }

        // Keep the background pixels that have not yet been
        // removed.
        for (uint16_t i = requestedOff;
             i < startupTransitionOffCount;
             ++i)
        {
            const uint8_t id =
                startupTransitionOffOrder[i];

            frame[id] = true;
        }

        renderNodeFrame(frame);

        lastTransitionOffShown = requestedOff;
    }


    // --------------------------------------------------------
    // STAGE 5B: GRADUALLY CHANGE THE PULSE SPEED
    // --------------------------------------------------------

    /*
     * Blend the startup and heart pulse phases.
     *
     * At the beginning:
     *     Startup period = 1500ms.
     *
     * At the end:
     *     Heart period = 900ms.
     *
     * The transition lasts 1800ms, which is exactly two
     * final heart-pulse periods.
     *
     * Therefore, the transition finishes at the minimum
     * brightness of the heart pulse.
     */

    const float startupPhase =
        2.0f * 3.14159265358979323846f *
        (
            (float)transitionElapsedMs /
            (float)STARTUP_PULSE_PERIOD_MS
        );

    const float heartPhase =
        2.0f * 3.14159265358979323846f *
        (
            (float)transitionElapsedMs /
            HEART_PULSE_PERIOD_MS
        );

    // Smoothly blend the two pulse phases.
    const float phase =
        startupPhase +
        (
            (heartPhase - startupPhase) *
            easedProgress
        );

    const float pulse =
        0.5f - (0.5f * cosf(phase));


    // --------------------------------------------------------
    // STAGE 5C: BLEND THE BRIGHTNESS CURVE
    // --------------------------------------------------------

    // Gradually change the minimum brightness from startup
    // breathing brightness to the heart animation brightness.
    const float minimumFactor =
        STARTUP_PULSE_MIN_FACTOR +
        (
            (HEART_PULSE_MIN_FACTOR -
             STARTUP_PULSE_MIN_FACTOR) *
            easedProgress
        );

    // Gradually change the brightness reference as well.
    const float baseBrightness =
        (float)startupBaseBrightness +
        (
            ((float)heartBaseBrightness -
             (float)startupBaseBrightness) *
            easedProgress
        );

    const float brightnessFactor =
        minimumFactor +
        (
            (1.0f - minimumFactor) *
            pulse
        );

    uint8_t brightness =
        (uint8_t)lroundf(
            baseBrightness * brightnessFactor
        );

    // MAX7219 intensity range is 0..15.
    if (brightness > 15) {
        brightness = 15;
    }

    // Preserve the OFF state if the interpolated base is zero.
    if (baseBrightness > 0.0f && brightness == 0) {
        brightness = 1;
    }

    if (brightness != lastStartupBrightness)
    {
        MAX7219_SetBrightness(brightness);
        lastStartupBrightness = brightness;
    }


    // --------------------------------------------------------
    // STARTUP -> HEART HANDOFF
    // --------------------------------------------------------

    if (transitionElapsedMs >= STARTUP_TRANSITION_DURATION_MS)
    {
        // Make sure the final pattern contains only the hearts.
        if (lastTransitionOffShown != startupTransitionOffCount)
        {
            bool frame[NODE_COUNT] = {};

            for (uint16_t id = 0; id < NODE_COUNT; ++id)
            {
                if (startupHeartMask[id]) {
                    frame[id] = true;
                }
            }

            renderNodeFrame(frame);

            lastTransitionOffShown =
                startupTransitionOffCount;
        }

        // Explicitly set the exact HEART starting brightness.
        const uint8_t heartStartingBrightness =
            (uint8_t)lroundf(
                (float)heartBaseBrightness *
                HEART_PULSE_MIN_FACTOR
            );

        MAX7219_SetBrightness(heartStartingBrightness);

        lastStartupBrightness =
            heartStartingBrightness;

        // Tell setEffectInternal() to preserve this brightness
        // when AnimationManager switches STARTUP to HEART.
        startupHandoffPending = true;

        startupComplete = true;

        Serial.println(
            "[SMALL ANIMATIONS] Smooth HEART transition completed"
        );
    }
}

// ============================================================
// HEART ANIMATION
// ============================================================

/*
 * RENDER DOUBLE HEART
 *
 * Draw the same Icon_Heart bitmap on both physical modules.
 *
 * UPPER SLAVE:
 *     X = 0..7.
 *
 * LOWER MASTER:
 *     X = 8..15.
 *
 * The original 8x8 icon bitmap is used without scaling
 * or distortion.
 *
 * Physical 45-degree module mounting automatically
 * determines the orientation of each displayed heart.
 */
void renderHeart()
{
    // Protect against missing or invalid icon data.
    if (Icon_Heart.data == nullptr ||
        Icon_Heart.width != 8 ||
        Icon_Heart.height != 8)
    {
        Serial.println("[SMALL ANIMATIONS] Invalid heart icon");
        return;
    }

    MAX7219_Clear();

    // Draw the heart bitmap on both modules.
    for (uint8_t y = 0; y < 8; ++y)
    {
        const uint8_t row = Icon_Heart.data[y];

        for (uint8_t x = 0; x < 8; ++x)
        {
            // Icon convention: MSB is the leftmost pixel.
            const bool pixelOn =
                (row & (uint8_t)(0x80U >> x)) != 0;

            if (!pixelOn) {
                continue;
            }

            // Upper slave heart.
            MAX7219_SetPixel(x, y, 1);

            // Lower master heart.
            MAX7219_SetPixel(x + 8, y, 1);
        }
    }

    MAX7219_Update();
}

/*
 * UPDATE DOUBLE HEART
 *
 * Both heart icons retain their exact 8x8 bitmap.
 *
 * The heartbeat effect is created by changing MAX7219
 * intensity rather than resizing or distorting the icons.
 */
void updateHeart(uint32_t nowMs)
{
    if ((uint32_t)(nowMs - lastEffectRenderMs) >=
        HEART_UPDATE_MS)
    {
        lastEffectRenderMs = nowMs;

        const float elapsedMs =
            (float)(uint32_t)(nowMs - effectStartedMs);

        const float phase =
            (2.0f * 3.14159265358979323846f) *
            (elapsedMs / HEART_PULSE_PERIOD_MS);

        const float pulse =
            0.5f - (0.5f * cosf(phase));

        const float brightnessFactor =
            0.55f + (0.45f * pulse);

        uint8_t brightness =
            (uint8_t)lroundf(
                heartBaseBrightness * brightnessFactor
            );

        // Preserve the OFF state if the original brightness is zero.
        if (heartBaseBrightness > 0 && brightness == 0) {
            brightness = 1;
        }

        MAX7219_SetBrightness(brightness);
    }
}

// ============================================================
// GRAVITY ESTIMATION
// ============================================================

/*
 * UPDATE GRAVITY DIRECTION
 *
 * Uses the same sensor mapping as WaterAnimation
 * and SandGravity.
 *
 * Gravity X = +AY.
 * Gravity Y = -AZ.
 *
 * MPU X gyroscope data tracks rotation around the screen normal.
 *
 * Accelerometer corrects long-term gyroscope drift.
 *
 * Small animations do not rotate the framebuffer.
 * Instead, their gravity-dependent effects use this vector
 * in the common physical coordinate system.
 */
void updateGravity()
{
    MPU6050::Data imu{};

    if (!MPU6050::getData(imu) || !imu.valid) {
        return;
    }

    // Avoid processing the same IMU snapshot repeatedly.
    if (imuTimestampInitialized &&
        imu.timestampMs == lastImuTimestampMs)
    {
        return;
    }

    float dt = 0.01f;

    if (imuTimestampInitialized)
    {
        const uint32_t elapsedMs =
            (uint32_t)(imu.timestampMs - lastImuTimestampMs);

        if (elapsedMs > 0) {
            dt = elapsedMs * 0.001f;
        }
    }

    dt = clampFloat(dt, 0.001f, 0.05f);

    lastImuTimestampMs = imu.timestampMs;
    imuTimestampInitialized = true;

    // Gravity projection onto the physical display plane.
    const float accelX =
        imu.ayG * FLOW_X_SIGN;

    const float accelY =
        -imu.azG * FLOW_Y_SIGN;

    const float projectionMagnitude =
        sqrtf(accelX * accelX + accelY * accelY);

    const float totalAcceleration =
        sqrtf(
            imu.axG * imu.axG +
            imu.ayG * imu.ayG +
            imu.azG * imu.azG
        );

    // Accelerometer confidence is reduced during strong
    // linear acceleration.
    const float accelerationError =
        fabsf(totalAcceleration - 1.0f);

    const float confidence =
        clampFloat(
            1.0f - accelerationError / 0.5f,
            0.0f,
            1.0f
        );

    if (!gravityInitialized)
    {
        if (projectionMagnitude > 0.05f)
        {
            gravityAngleRad =
                atan2f(accelX, accelY);

            gravityMagnitude =
                projectionMagnitude;
        }
        else
        {
            gravityAngleRad = 0.0f;
            gravityMagnitude = 0.0f;
        }

        gravityInitialized = true;
    }
    else
    {
        // Gyroscope integration.
        const float gyroRate =
            imu.gxDps *
            RADIANS_PER_DEGREE *
            GYRO_X_SIGN;

        gravityAngleRad += gyroRate * dt;

        gravityAngleRad =
            normalizeAngle(gravityAngleRad);

        // Accelerometer correction.
        if (projectionMagnitude > 0.05f &&
            confidence > 0.01f)
        {
            const float accelAngle =
                atan2f(accelX, accelY);

            const float error =
                atan2f(
                    sinf(accelAngle - gravityAngleRad),
                    cosf(accelAngle - gravityAngleRad)
                );

            const float correction =
                (1.0f - expf(-dt / ACCEL_CORRECTION_TAU)) *
                confidence;

            gravityAngleRad += error * correction;

            gravityAngleRad =
                normalizeAngle(gravityAngleRad);
        }

        // Filter gravity magnitude.
        if (confidence > 0.1f)
        {
            const float alpha =
                1.0f - expf(-dt / GRAVITY_MAGNITUDE_TAU);

            gravityMagnitude +=
                alpha * confidence *
                (projectionMagnitude - gravityMagnitude);
        }
    }

    gravityMagnitude =
        clampFloat(gravityMagnitude, 0.0f, 1.5f);

    gravityX =
        sinf(gravityAngleRad) * gravityMagnitude;

    gravityY =
        cosf(gravityAngleRad) * gravityMagnitude;
}

// ============================================================
// RAIN ANIMATION
// ============================================================

/*
 * FIND UPSTREAM NODES
 *
 * Rain starts from the boundary opposite to gravity.
 *
 * When upright:
 *     Rain originates from the upper physical boundary.
 *
 * When upside down:
 *     Rain originates from the opposite boundary.
 *
 * When sideways:
 *     Rain originates from the corresponding side.
 *
 * The physical node coordinates determine the spawn edge.
 */
uint8_t findUpstreamNodes(uint8_t *candidateNodes)
{
    float minProjection = 100000.0f;

    // First find the minimum gravity projection.
    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        const WaterGeometry::Node *node =
            getPhysicalNode((uint8_t)id);

        if (node == nullptr) {
            continue;
        }

        const float projection =
            node->x * gravityX +
            node->y * gravityY;

        minProjection =
            fminf(minProjection, projection);
    }

    // Collect nodes near the upstream edge.
    uint8_t count = 0;

    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        const WaterGeometry::Node *node =
            getPhysicalNode((uint8_t)id);

        if (node == nullptr) {
            continue;
        }

        const float projection =
            node->x * gravityX +
            node->y * gravityY;

        if (projection <=
            minProjection + RAIN_UPSTREAM_BAND)
        {
            candidateNodes[count++] =
                (uint8_t)id;
        }
    }

    return count;
}

/*
 * CHECK WHETHER A NODE ALREADY HAS A RAIN DROP.
 */
bool isRainNodeOccupied(uint8_t nodeID)
{
    for (uint8_t i = 0; i < MAX_RAIN_DROPS; ++i)
    {
        if (rainDrops[i].active &&
            rainDrops[i].nodeID == nodeID)
        {
            return true;
        }
    }

    return false;
}

/*
 * SPAWN ONE RAIN DROP
 *
 * Select a random node near the physical upstream boundary.
 */
void spawnRainDrop(uint32_t nowMs)
{
    int8_t freeSlot = -1;

    for (uint8_t i = 0; i < MAX_RAIN_DROPS; ++i)
    {
        if (!rainDrops[i].active)
        {
            freeSlot = (int8_t)i;
            break;
        }
    }

    if (freeSlot < 0) {
        return;
    }

    uint8_t candidates[NODE_COUNT] = {};

    const uint8_t candidateCount =
        findUpstreamNodes(candidates);

    if (candidateCount == 0) {
        return;
    }

    // Try several random candidates to avoid overlapping drops.
    for (uint8_t attempt = 0; attempt < candidateCount; ++attempt)
    {
        const uint8_t randomIndex =
            (uint8_t)(
                esp_random() % candidateCount
            );

        const uint8_t nodeID =
            candidates[randomIndex];

        if (isRainNodeOccupied(nodeID)) {
            continue;
        }

        RainDrop &drop =
            rainDrops[(uint8_t)freeSlot];

        drop.nodeID = nodeID;
        drop.previousNodeID = 0xFF;

        drop.hasPrevious = false;
        drop.active = true;

        drop.lastMoveMs = nowMs;
        drop.bornMs = nowMs;

        return;
    }
}

/*
 * FIND NEXT RAIN NODE
 *
 * Find a physical neighbour that follows gravity most closely.
 *
 * Previous node is excluded when another valid direction exists.
 * This prevents a drop from repeatedly reversing between two LEDs.
 */
int16_t findNextRainNode(const RainDrop &drop)
{
    const WaterGeometry::Node *current =
        getPhysicalNode(drop.nodeID);

    if (current == nullptr) {
        return -1;
    }

    int16_t bestNode = -1;

    float bestAlignment = 0.12f;

    for (uint8_t i = 0;
         i < current->neighbourCount;
         ++i)
    {
        const uint8_t neighbourID =
            current->neighbours[i];

        // Prevent immediate reversal.
        if (drop.hasPrevious &&
            neighbourID == drop.previousNodeID)
        {
            continue;
        }

        const WaterGeometry::Node *next =
            getPhysicalNode(neighbourID);

        if (next == nullptr) {
            continue;
        }

        const float dx =
            next->x - current->x;

        const float dy =
            next->y - current->y;

        const float distance =
            sqrtf(dx * dx + dy * dy);

        if (distance < 0.001f) {
            continue;
        }

        const float alignment =
            (
                dx * gravityX +
                dy * gravityY
            ) / distance;

        if (alignment > bestAlignment)
        {
            bestAlignment = alignment;
            bestNode = neighbourID;
        }
    }

    return bestNode;
}

/*
 * UPDATE ONE RAIN DROP
 */
void updateRainDrop(RainDrop &drop, uint32_t nowMs)
{
    if (!drop.active) {
        return;
    }

    // Remove old drops that have existed for too long.
    if ((uint32_t)(nowMs - drop.bornMs) >=
        RAIN_MAX_LIFETIME_MS)
    {
        drop.active = false;
        return;
    }

    if ((uint32_t)(nowMs - drop.lastMoveMs) <
        RAIN_MOVE_INTERVAL_MS)
    {
        return;
    }

    drop.lastMoveMs = nowMs;

    const int16_t nextNode =
        findNextRainNode(drop);

    // No downhill connection is available.
    if (nextNode < 0)
    {
        drop.active = false;
        return;
    }

    // Advance the drop by one physical neighbour.
    drop.previousNodeID = drop.nodeID;
    drop.hasPrevious = true;

    drop.nodeID = (uint8_t)nextNode;
}

/*
 * RENDER RAIN FRAME
 *
 * Each drop illuminates:
 *
 *     1. Current position.
 *     2. Previous position as a short trail.
 */
void renderRain()
{
    bool frame[NODE_COUNT] = {};

    for (uint8_t i = 0; i < MAX_RAIN_DROPS; ++i)
    {
        const RainDrop &drop =
            rainDrops[i];

        if (!drop.active) {
            continue;
        }

        frame[drop.nodeID] = true;

        if (drop.hasPrevious &&
            drop.previousNodeID < NODE_COUNT)
        {
            frame[drop.previousNodeID] = true;
        }
    }

    renderNodeFrame(frame);
}

/*
 * UPDATE RAIN EFFECT
 *
 * Rain follows the current gravity direction through the
 * physical neighbour graph.
 *
 * A sudden device rotation changes the future path selection
 * without rotating or clearing the complete LED matrix.
 */
void updateRain(uint32_t nowMs)
{
    bool frameChanged = false;

    // Update existing drops.
    for (uint8_t i = 0; i < MAX_RAIN_DROPS; ++i)
    {
        const uint8_t oldNode =
            rainDrops[i].nodeID;

        const bool oldActive =
            rainDrops[i].active;

        updateRainDrop(rainDrops[i], nowMs);

        if (oldNode != rainDrops[i].nodeID ||
            oldActive != rainDrops[i].active)
        {
            frameChanged = true;
        }
    }

    // Spawn new rain drops periodically.
    if ((uint32_t)(nowMs - lastRainSpawnMs) >=
        RAIN_SPAWN_INTERVAL_MS)
    {
        lastRainSpawnMs = nowMs;

        spawnRainDrop(nowMs);

        frameChanged = true;
    }

    // Render at controlled intervals.
    if (frameChanged ||
        (uint32_t)(nowMs - lastRainUpdateMs) >= RAIN_MOVE_INTERVAL_MS)
    {
        lastRainUpdateMs = nowMs;

        renderRain();
    }
}

// ============================================================
// WAVE ANIMATION
// ============================================================

/*
 * RENDER WAVE
 *
 * A travelling wave uses the gravity projection of each
 * physical LED node.
 *
 * Wave crests are perpendicular to the gravity direction.
 *
 * The wave direction therefore changes naturally when
 * the device is tilted or rotated.
 *
 * No display framebuffer rotation is performed.
 */
void renderWave()
{
    bool frame[NODE_COUNT] = {};

    // Use the screen-down direction if gravity projection
    // is extremely small.
    float gx = gravityX;
    float gy = gravityY;

    const float magnitude =
        sqrtf(gx * gx + gy * gy);

    if (magnitude < 0.05f)
    {
        gx = 0.0f;
        gy = 1.0f;
    }
    else
    {
        gx /= magnitude;
        gy /= magnitude;
    }

    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        const WaterGeometry::Node *node =
            getPhysicalNode((uint8_t)id);

        if (node == nullptr) {
            continue;
        }

        // Physical distance along the current gravity direction.
        const float projection =
            node->x * gx +
            node->y * gy;

        // Generate a travelling sinusoidal wave.
        const float phase =
            (projection * WAVE_FREQUENCY) -
            wavePhase;

        const float waveValue =
            sinf(phase) +
            (0.25f * sinf(phase * 2.0f));

        if (waveValue > WAVE_THRESHOLD) {
            frame[id] = true;
        }
    }

    renderNodeFrame(frame);
}

/*
 * UPDATE WAVE EFFECT
 */
void updateWaves(uint32_t nowMs)
{
    if ((uint32_t)(nowMs - lastEffectRenderMs) >=
        WAVE_UPDATE_MS)
    {
        lastEffectRenderMs = nowMs;

        const float elapsedMs =
            (float)(uint32_t)(nowMs - effectStartedMs);

        wavePhase =
            elapsedMs * WAVE_SPEED;

        renderWave();
    }
}

// ============================================================
// EFFECT CONTROL
// ============================================================

/*
 * SET EFFECT INTERNAL
 *
 * Changes the active small animation and resets
 * its individual state.
 *
 * SPECIAL STARTUP -> HEART HANDOFF:
 *
 * When startup finishes its background-removal transition:
 *     - Keep the existing heart framebuffer.
 *     - Preserve the final transition brightness.
 *     - Do not redraw or restart the heart visually.
 *
 * For all other transitions, initialize the selected
 * effect normally.
 *
 * The caller must already have initialized WaterGeometry.
 */
void setEffectInternal(Effect effect, uint32_t nowMs)
{
    const Effect previousEffect = currentEffect;

    // --------------------------------------------------------
    // CHECK FOR THE SPECIAL STARTUP -> HEART HANDOFF
    // --------------------------------------------------------

    // This must be checked before changing startupComplete
    // or startupHandoffPending.
    const bool smoothStartupHandoff =
        previousEffect == Effect::STARTUP &&
        effect == Effect::HEART &&
        startupComplete &&
        startupHandoffPending;


    // --------------------------------------------------------
    // RESTORE THE PREVIOUS EFFECT'S NORMAL BRIGHTNESS
    // --------------------------------------------------------

    if (previousEffect == Effect::HEART &&
        effect != Effect::HEART)
    {
        MAX7219_SetBrightness(heartBaseBrightness);
    }
    else if (previousEffect == Effect::STARTUP &&
             effect != Effect::STARTUP &&
             !smoothStartupHandoff &&
             startupBrightnessCaptured)
    {
        // Ordinary interruption of startup.
        //
        // Do not restore brightness during the smooth
        // STARTUP -> HEART handoff.
        MAX7219_SetBrightness(startupBaseBrightness);
    }


    // --------------------------------------------------------
    // HANDLE STARTUP BRIGHTNESS CAPTURE
    // --------------------------------------------------------

    if (effect == Effect::STARTUP)
    {
        // Restarting an existing startup animation.
        // Restore its original brightness before capturing it.
        if (previousEffect == Effect::STARTUP &&
            startupBrightnessCaptured)
        {
            MAX7219_SetBrightness(startupBaseBrightness);
        }

        // Capture the normal matrix brightness.
        startupBaseBrightness = MAX7219_GetBrightness();

        startupBrightnessCaptured = true;

        startupComplete = false;
        startupHandoffPending = false;

        lastStartupBrightness = 0xFFU;
        lastTransitionOffShown = 0xFFFFU;
    }
    else if (previousEffect == Effect::STARTUP)
    {
        // Startup has now handed control to another effect.
        // Its original brightness no longer needs to be restored
        // by end() or a later effect transition.
        startupBrightnessCaptured = false;

        startupComplete = false;
    }


    // --------------------------------------------------------
    // UPDATE ACTIVE EFFECT STATE
    // --------------------------------------------------------

    currentEffect = effect;

    effectStartedMs = nowMs;
    lastEffectRenderMs = 0;


    // --------------------------------------------------------
    // INITIALIZE THE SELECTED EFFECT
    // --------------------------------------------------------

    switch (effect)
    {
        case Effect::STARTUP:
        {
            lastBorderShown = 0xFFFFU;
            lastFillShown = 0xFFFFU;

            startupComplete = false;
            startupHandoffPending = false;

            break;
        }


        case Effect::HEART:
        {
            if (!smoothStartupHandoff)
            {
                // Normal entry into HEART.
                renderHeart();

                // Start at the minimum heartbeat brightness.
                const uint8_t startingBrightness =
                    (uint8_t)lroundf(
                        heartBaseBrightness *
                        HEART_PULSE_MIN_FACTOR
                    );

                MAX7219_SetBrightness(startingBrightness);
            }

            /*
             * During a smooth startup handoff:
             *
             * Do not call renderHeart().
             *
             * The final transition frame already contains
             * exactly the two heart icons.
             *
             * The brightness is also already at the correct
             * starting point for updateHeart().
             */

            startupHandoffPending = false;
            startupComplete = false;

            break;
        }


        case Effect::RAIN:
        {
            // Remove all previous rain drops.
            memset(rainDrops, 0, sizeof(rainDrops));

            lastRainSpawnMs = nowMs;
            lastRainUpdateMs = nowMs;

            startupHandoffPending = false;

            break;
        }


        case Effect::WAVES:
        {
            wavePhase = 0.0f;

            startupHandoffPending = false;

            break;
        }


        default:
        {
            break;
        }
    }


    Serial.printf(
        "[SMALL ANIMATIONS] Effect: %u\n",
        (unsigned int)effect
    );
}

// ============================================================
// RESET EFFECT STATE
// ============================================================

void resetAnimationState()
{
    memset(rainDrops, 0, sizeof(rainDrops));

    startupHandoffPending = false;
    lastTransitionOffShown = 0xFFFFU;

    lastBorderShown = 0xFFFFU;
    lastFillShown = 0xFFFFU;

    startupComplete = false;
    lastStartupBrightness = 0xFFU;

    lastRainSpawnMs = 0;
    lastRainUpdateMs = 0;

    wavePhase = 0.0f;
}

// ============================================================
// PUBLIC API
// ============================================================

} // namespace

// ------------------------------------------------------------
// INITIALIZATION
// ------------------------------------------------------------

bool begin()
{
    if (initialized) {
        return true;
    }

    // Build or reuse the shared physical geometry.
    if (!WaterGeometry::begin())
    {
        Serial.println(
            "[SMALL ANIMATIONS] Geometry initialization failed"
        );

        return false;
    }

    // Calculate the physical bounding box.
    if (!calculateGeometryBounds())
    {
        Serial.println(
            "[SMALL ANIMATIONS] Invalid physical geometry"
        );

        return false;
    }

    // Build the physical startup border sequence.
    if (!buildBorderOrder())
    {
        Serial.println(
            "[SMALL ANIMATIONS] Border generation failed"
        );

        return false;
    }

    // Build the centre-outward interior fill sequence.
    if (!buildStartupFillOrder())
    {
        Serial.println(
            "[SMALL ANIMATIONS] Interior fill generation failed"
        );

        return false;
    }

    if (!buildStartupTransitionOrder()) {

        Serial.println(
            "[SMALL ANIMATIONS] Interior transition generation failed"
        );

        return false;
    }

    resetAnimationState();

    // Begin with the startup border animation.
    initialized = true;

    setEffectInternal(Effect::STARTUP, millis());

    Serial.printf(
        "[SMALL ANIMATIONS] Geometry ready: %u nodes, "
        "%u border LEDs, %u interior LEDs\n",
        (unsigned int)NODE_COUNT,
        (unsigned int)borderCount,
        (unsigned int)startupFillCount
    );

    return true;
}

// ------------------------------------------------------------
// RESET
// ------------------------------------------------------------

void reset(uint32_t nowMs)
{
    if (!initialized)
    {
        if (!begin()) {
            return;
        }
    }

    resetAnimationState();

    // Preserve gravity state from the sensor.
    setEffectInternal(Effect::STARTUP, nowMs);
}

// ------------------------------------------------------------
// SELECT EFFECT
// ------------------------------------------------------------

bool setEffect(Effect effect)
{
    if (!initialized) {
        return false;
    }

    setEffectInternal(effect, millis());

    return true;
}

// ------------------------------------------------------------
// NEXT EFFECT
// ------------------------------------------------------------

bool nextEffect()
{
    if (!initialized) {
        return false;
    }

    Effect next;

    switch (currentEffect)
    {
        case Effect::STARTUP:
            next = Effect::HEART;
            break;

        case Effect::HEART:
            next = Effect::RAIN;
            break;

        case Effect::RAIN:
            next = Effect::WAVES;
            break;

        case Effect::WAVES:
        default:
            next = Effect::HEART;
            break;
    }

    return setEffect(next);
}

// ------------------------------------------------------------
// CURRENT EFFECT
// ------------------------------------------------------------

Effect getEffect()
{
    return currentEffect;
}

// ------------------------------------------------------------
// UPDATE
// ------------------------------------------------------------

void update(uint32_t nowMs)
{
    if (!initialized)
    {
        if (!begin()) {
            return;
        }
    }

    // Update gravity continuously.
    // Rain and waves use this physical direction.
    // The heart and startup effects do not need to rotate.
    updateGravity();

    switch (currentEffect)
    {
        case Effect::STARTUP:
        {
            updateStartup(nowMs);
            break;
        }

        case Effect::HEART:
        {
            updateHeart(nowMs);
            break;
        }

        case Effect::RAIN:
        {
            updateRain(nowMs);
            break;
        }

        case Effect::WAVES:
        {
            updateWaves(nowMs);
            break;
        }

        default:
        {
            setEffectInternal(Effect::HEART, nowMs);
            break;
        }
    }
}

// ------------------------------------------------------------
// STOP
// ------------------------------------------------------------

void end()
{
    // Restore normal brightness if HEART was active.
    if (currentEffect == Effect::HEART)
    {
        MAX7219_SetBrightness(heartBaseBrightness);
    }
    // Also restore normal brightness if STARTUP was interrupted.
    else if (currentEffect == Effect::STARTUP &&
             startupBrightnessCaptured)
    {
        MAX7219_SetBrightness(startupBaseBrightness);
    }

    startupBrightnessCaptured = false;

    initialized = false;

    resetAnimationState();
    currentEffect = Effect::STARTUP;

    // MAX7219 is controlled by AnimationManager.
    // Do not clear or update its framebuffer here.
}

// ------------------------------------------------------------
// STATUS
// ------------------------------------------------------------

bool isReady()
{
    return initialized && WaterGeometry::isReady();
}

bool isStartupComplete()
{
    return initialized && startupComplete;
}

} // namespace SmallAnimations

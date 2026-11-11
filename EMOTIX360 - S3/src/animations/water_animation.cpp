
#include "water_animation.h"

#include "max7219.h"
#include "mpu6050.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace WaterAnimation {
namespace {

// ------------------------------------------------------------
// PHYSICAL MATRIX CONFIGURATION
// ------------------------------------------------------------

// The two 8x8 modules are physically stacked:
//
//     Rows 0..7    = Upper slave module.
//     Rows 8..15   = Lower master module.
//
// Simulation coordinates:
//     X = 0..7     Horizontal position.
//     Y = 0..15    Vertical position.
//
// This represents the actual physical 8x16 hourglass.

constexpr uint8_t MATRIX_WIDTH = 8;
constexpr uint8_t MATRIX_HEIGHT = 16;

constexpr uint16_t CELL_TOTAL =
    MATRIX_WIDTH * MATRIX_HEIGHT;

constexpr uint8_t WATER_TOTAL = 64;

// ------------------------------------------------------------
// WATER ANIMATION TIMING
// ------------------------------------------------------------

// Time between liquid movement updates.
constexpr uint32_t FLOW_STEP_MS = 55;

// Maximum water pixels allowed to move during one flow update.
constexpr uint8_t MAX_MOVES_PER_STEP = 4;

// Minimum downhill movement required before a pixel moves.
constexpr float MIN_FLOW_PROJECTION = 0.12f;

// Gravity-vector low-pass filtering.
// Higher values respond faster but can produce more jitter.
constexpr float GRAVITY_FILTER_ALPHA = 0.35f;

// ------------------------------------------------------------
// AUTOMATIC DISPLAY ORIENTATION
// ------------------------------------------------------------

// These thresholds match SandClock orientation detection.
constexpr int16_t GRAVITY_THRESHOLD = 10000;
constexpr int16_t DIAGONAL_THRESHOLD = 9000;

constexpr uint32_t POSITION_STABLE_MS = 400;

// Current complete matrix orientation.
bool upsideDown = false;

// Candidate orientation being checked for stability.
bool candidateUpsideDown = false;
bool orientationCandidateValid = false;

uint32_t candidateOrientationSinceMs = 0;

// ------------------------------------------------------------
// WATER FRAMEBUFFER
// ------------------------------------------------------------

// Physical 8x16 framebuffer.
//
// water[y][x]:
//     true  = water pixel ON.
//     false = empty position.
//
// Total number of occupied cells must remain 64.
bool water[MATRIX_HEIGHT][MATRIX_WIDTH] = {};

// Animation state.
bool initialized = false;

uint32_t lastFlowStepMs = 0;

// Rotating scan start prevents movement selection from
// consistently favouring the same framebuffer coordinates.
uint16_t scanCursor = 0;

// ------------------------------------------------------------
// HOURGLASS TANK SHAPE
// ------------------------------------------------------------

/*
 * WIDTH OF EACH CHAMBER ROW
 *
 * Each chamber starts wide at its outer end and narrows
 * towards the central connection.
 *
 * Row 0: 8 pixels.
 * Row 1: 8 pixels.
 * Row 2: 6 pixels.
 * Row 3: 6 pixels.
 * Row 4: 4 pixels.
 * Row 5: 4 pixels.
 * Row 6: 2 pixels.
 * Row 7: 2 pixels.
 *
 * The lower chamber uses the same shape mirrored vertically.
 *
 * Total permitted cells:
 *     40 upper + 40 lower = 80 cells.
 *
 * Water volume:
 *     64 cells.
 */
constexpr uint8_t CHAMBER_WIDTH[MATRIX_WIDTH] = {
    8, 8, 6, 6, 4, 4, 2, 2
};

/*
 * CHECK TANK BOUNDARY
 *
 * Returns true only when a coordinate is inside the
 * hourglass-shaped liquid container.
 *
 * The narrowest point is between rows 7 and 8.
 */
bool isInsideTank(uint8_t x, uint8_t y)
{
    if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) {
        return false;
    }

    // Convert the lower chamber row into its mirrored
    // upper chamber row.
    const uint8_t chamberRow =
        (y < 8) ? y : (uint8_t)(15 - y);

    const uint8_t allowedWidth =
        CHAMBER_WIDTH[chamberRow];

    const uint8_t leftBoundary =
        (uint8_t)((MATRIX_WIDTH - allowedWidth) / 2);

    const uint8_t rightBoundary =
        (uint8_t)(leftBoundary + allowedWidth);

    return (x >= leftBoundary && x < rightBoundary);
}

// ------------------------------------------------------------
// AUTOMATIC DISPLAY ORIENTATION
// ------------------------------------------------------------

/*
 * UPDATE WATER DISPLAY ORIENTATION
 *
 * Uses the same accelerometer thresholds and stability
 * period as SandClock.
 *
 * Normal position:
 *     AZ < -10000.
 *
 * Upside-down position:
 *     AZ > +10000.
 *
 * Diagonal position:
 *     AX > 9000 && AY > 9000.
 *
 * Sideways and transition positions are ignored for
 * display rotation.
 *
 * Water gravity calculation is NOT ignored during sideways
 * movement. The liquid must continue responding to tilt.
 *
 * Returns true when the active display orientation changes.
 */
bool updateUpsideDownOrientation(
    const MPU6050::Data &imu,
    uint32_t nowMs
)
{
    // Match SandClock's diagonal-position detection precedence.
    if (imu.axRaw > DIAGONAL_THRESHOLD &&
        imu.ayRaw > DIAGONAL_THRESHOLD)
    {
        orientationCandidateValid = false;
        return false;
    }

    bool detectedUpsideDown;

    if (imu.azRaw > GRAVITY_THRESHOLD)
    {
        detectedUpsideDown = true;
    }
    else if (imu.azRaw < -GRAVITY_THRESHOLD)
    {
        detectedUpsideDown = false;
    }
    else
    {
        // Ignore sideways and transition positions.
        orientationCandidateValid = false;
        return false;
    }

    // Start checking a new candidate orientation.
    if (!orientationCandidateValid ||
        detectedUpsideDown != candidateUpsideDown)
    {
        candidateUpsideDown = detectedUpsideDown;
        candidateOrientationSinceMs = nowMs;
        orientationCandidateValid = true;

        return false;
    }

    // Wait until the candidate orientation remains stable.
    if ((uint32_t)(nowMs - candidateOrientationSinceMs) <
        POSITION_STABLE_MS)
    {
        return false;
    }

    // Change the complete display orientation only when necessary.
    if (upsideDown != candidateUpsideDown)
    {
        upsideDown = candidateUpsideDown;
        return true;
    }

    return false;
}

// ------------------------------------------------------------
// GRAVITY DIRECTION
// ------------------------------------------------------------

// Gravity in the physical simulation coordinate system.
//
// X = horizontal direction.
// Y = vertical direction (positive means upper to lower).
//
// Initial upright gravity points towards the lower chamber.
float gravityX = 0.0f;
float gravityY = 1.0f;

bool gravityInitialized = false;

// Signs can be adjusted if physical movement is reversed.
constexpr float FLOW_X_SIGN = 1.0f;
constexpr float FLOW_Y_SIGN = 1.0f;

/*
 * UPDATE GRAVITY DIRECTION
 *
 * MPU6050 mounting in EMOTIX360:
 *
 *     X+ points towards the user (normal to the display).
 *     Y+ points towards the left.
 *     Z+ points downwards in the normal device position.
 *
 * Therefore, Y and Z are the two useful accelerometer
 * axes for estimating gravity on the display plane.
 *
 * The MPU6050's measured acceleration is opposite to the
 * physical gravity direction at rest.
 *
 * Current confirmed normal position:
 *     AZ is negative.
 *
 * Mapping:
 *     Gravity X = +AY.
 *     Gravity Y = -AZ.
 *
 * FLOW_X_SIGN and FLOW_Y_SIGN allow calibration if the
 * physical matrix mapping requires reversing an axis.
 *
 * When the device turns upside down, the accelerometer
 * vector itself changes direction. Do not reverse it again
 * using upsideDown; that would double-invert gravity.
 */
void updateGravityDirection(const MPU6050::Data &imu)
{
    // Calculate the in-plane gravity vector.
    float gx = imu.ayG * FLOW_X_SIGN;
    float gy = -imu.azG * FLOW_Y_SIGN;

    // Normalize the vector.
    const float magnitude =
        sqrtf((gx * gx) + (gy * gy));

    // When gravity is nearly perpendicular to the display,
    // there is no meaningful in-plane flow direction.
    if (magnitude < 0.08f)
    {
        gravityX = 0.0f;
        gravityY = 0.0f;
        gravityInitialized = true;

        return;
    }

    gx /= magnitude;
    gy /= magnitude;

    // Initialize the filter using the first valid sensor sample.
    if (!gravityInitialized)
    {
        gravityX = gx;
        gravityY = gy;

        gravityInitialized = true;
        return;
    }

    // Low-pass filter the gravity vector to reduce jitter.
    gravityX +=
        GRAVITY_FILTER_ALPHA * (gx - gravityX);

    gravityY +=
        GRAVITY_FILTER_ALPHA * (gy - gravityY);

    // Normalize the filtered vector again.
    const float filteredMagnitude =
        sqrtf(
            (gravityX * gravityX) +
            (gravityY * gravityY)
        );

    if (filteredMagnitude > 0.001f)
    {
        gravityX /= filteredMagnitude;
        gravityY /= filteredMagnitude;
    }
}

// ------------------------------------------------------------
// WATER INITIALIZATION
// ------------------------------------------------------------

/*
 * INITIAL WATER DISTRIBUTION
 *
 * The two chambers each contain 32 water pixels.
 *
 * Upper chamber:
 *     Fill rows 1..7, towards the central neck.
 *
 * Lower chamber:
 *     Fill rows 11..15, towards the outer bottom.
 *
 * Total:
 *     32 + 32 = 64 water pixels.
 *
 * Every occupied cell is inside the hourglass boundary.
 */
void initializeWater()
{
    memset(water, 0, sizeof(water));

    // Fill the upper chamber near the central neck.
    for (uint8_t y = 1; y < MATRIX_SIZE; ++y)
    {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x)
        {
            if (isInsideTank(x, y)) {
                water[y][x] = true;
            }
        }
    }

    // Fill the lower chamber towards its outer bottom.
    for (uint8_t y = 11; y < MATRIX_HEIGHT; ++y)
    {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x)
        {
            if (isInsideTank(x, y)) {
                water[y][x] = true;
            }
        }
    }
}

// ------------------------------------------------------------
// WATER PIXEL MAPPING
// ------------------------------------------------------------

/*
 * SET WATER PIXEL
 *
 * Input coordinates use the physical 8x16 simulation grid.
 *
 * Upper slave module:
 *     Simulation Y = 0..7.
 *
 * Lower master module:
 *     Simulation Y = 8..15.
 *
 * MAX7219 driver mapping:
 *     Upper slave: X = 0..7.
 *     Lower master: X = 8..15.
 *
 * Upside-down rotation uses the same transformation
 * already tested successfully in SandClock:
 *
 *     SLAVE:
 *         Swap to master.
 *         Rotate 90 degrees clockwise.
 *
 *     MASTER:
 *         Swap to slave.
 *         Rotate 90 degrees anticlockwise.
 */
void setWaterPixel(uint8_t x, uint8_t y, uint8_t state)
{
    if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) {
        return;
    }

    // Convert physical coordinates to MAX7219 driver coordinates.
    uint8_t driverX =
        (uint8_t)(x + ((y >= MATRIX_SIZE) ? MATRIX_SIZE : 0));

    uint8_t driverY =
        (uint8_t)(y % MATRIX_SIZE);

    if (upsideDown)
    {
        // Preserve the original coordinates before transformation.
        const uint8_t oldX = driverX;
        const uint8_t oldY = driverY;

        if (oldX < MATRIX_SIZE)
        {
            // ------------------------------------------------
            // SLAVE MODULE
            // Swap to master.
            // Rotate 90 degrees CLOCKWISE.
            // ------------------------------------------------

            driverX = (uint8_t)(
                MATRIX_SIZE + (MATRIX_SIZE - 1 - oldY)
            );

            driverY = oldX;
        }
        else
        {
            // ------------------------------------------------
            // MASTER MODULE
            // Swap to slave.
            // Rotate 90 degrees ANTICLOCKWISE.
            // ------------------------------------------------

            const uint8_t localX =
                (uint8_t)(oldX - MATRIX_SIZE);

            driverX = oldY;

            driverY = (uint8_t)(
                MATRIX_SIZE - 1 - localX
            );
        }
    }

    MAX7219_SetPixel(driverX, driverY, state);
}

// ------------------------------------------------------------
// FRAMEBUFFER RENDERING
// ------------------------------------------------------------

/*
 * RENDER WATER FRAME
 *
 * Draw every occupied cell in the physical 8x16 framebuffer.
 *
 * The water count remains exactly 64.
 */
void render()
{
    MAX7219_Clear();

    for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y)
    {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x)
        {
            if (water[y][x])
            {
                setWaterPixel(x, y, 1);
            }
        }
    }

    MAX7219_Update();
}

// ------------------------------------------------------------
// WATER MOVEMENT
// ------------------------------------------------------------

/*
 * TRY TO MOVE ONE WATER PIXEL
 *
 * A water pixel can move to one of its eight neighbouring cells.
 *
 *     NW   N   NE
 *      \   |   /
 *       \  |  /
 *     W -- P -- E
 *       /  |  \
 *      /   |   \
 *     SW   S   SE
 *
 * A movement is allowed only when:
 *
 * 1. The starting position contains water.
 * 2. The destination is inside the 8x16 framebuffer.
 * 3. The destination is inside the hourglass tank.
 * 4. The destination is currently empty.
 * 5. The movement has a sufficient downhill projection.
 *
 * The movement with the strongest normalized downhill
 * projection is selected.
 *
 * Exactly one water pixel moves during this function.
 *
 * Returns true when a pixel successfully moves.
 */
bool moveOneWaterPixel()
{
    constexpr int8_t NEIGHBOUR_X[8] = {
         0,  1, -1,
         1, -1,
         0,  1, -1
    };

    constexpr int8_t NEIGHBOUR_Y[8] = {
         1,  1,  1,
         0,  0,
        -1, -1, -1
    };

    float bestProjection = MIN_FLOW_PROJECTION;

    int8_t bestFromX = -1;
    int8_t bestFromY = -1;
    int8_t bestToX = -1;
    int8_t bestToY = -1;

    // Rotate scan start to reduce directional selection bias.
    const uint16_t scanStart = scanCursor;

    scanCursor = (uint16_t)(
        (scanCursor + 17) % CELL_TOTAL
    );

    // Search the complete physical 8x16 framebuffer.
    for (uint16_t scan = 0; scan < CELL_TOTAL; ++scan)
    {
        const uint16_t index =
            (uint16_t)((scanStart + scan) % CELL_TOTAL);

        const uint8_t fromX =
            (uint8_t)(index % MATRIX_WIDTH);

        const uint8_t fromY =
            (uint8_t)(index / MATRIX_WIDTH);

        // Only occupied cells can move.
        if (!water[fromY][fromX]) {
            continue;
        }

        // Check all eight neighbouring cells.
        for (uint8_t direction = 0;
             direction < 8;
             ++direction)
        {
            const int8_t dx = NEIGHBOUR_X[direction];
            const int8_t dy = NEIGHBOUR_Y[direction];

            const int16_t destinationX =
                (int16_t)fromX + dx;

            const int16_t destinationY =
                (int16_t)fromY + dy;

            // Destination must remain inside the framebuffer.
            if (destinationX < 0 ||
                destinationX >= MATRIX_WIDTH ||
                destinationY < 0 ||
                destinationY >= MATRIX_HEIGHT)
            {
                continue;
            }

            const uint8_t toX =
                (uint8_t)destinationX;

            const uint8_t toY =
                (uint8_t)destinationY;

            // The destination must be inside the tank.
            if (!isInsideTank(toX, toY)) {
                continue;
            }

            // Water cannot move into an occupied cell.
            if (water[toY][toX]) {
                continue;
            }

            // Calculate movement projection onto gravity.
            // Normalize diagonal movement to avoid diagonal bias.
            const float distance =
                (dx != 0 && dy != 0) ? 1.41421356f : 1.0f;

            const float projection =
                ((dx * gravityX) + (dy * gravityY)) / distance;

            // Reject sideways and uphill movements.
            if (projection <= bestProjection) {
                continue;
            }

            bestProjection = projection;

            bestFromX = (int8_t)fromX;
            bestFromY = (int8_t)fromY;

            bestToX = (int8_t)toX;
            bestToY = (int8_t)toY;
        }
    }

    // No valid downhill movement was found.
    if (bestFromX < 0 ||
        bestFromY < 0 ||
        bestToX < 0 ||
        bestToY < 0)
    {
        return false;
    }

    // Exchange one occupied cell with one empty cell.
    // The total water count is preserved.
    water[(uint8_t)bestFromY][(uint8_t)bestFromX] = false;
    water[(uint8_t)bestToY][(uint8_t)bestToX] = true;

    return true;
}

// ------------------------------------------------------------
// WATER SIMULATION STEP
// ------------------------------------------------------------

/*
 * UPDATE WATER FLOW
 *
 * Move up to MAX_MOVES_PER_STEP water pixels in the
 * current detected gravity direction.
 *
 * Each movement exchanges an occupied cell with an empty cell.
 * The total water count therefore remains exactly 64.
 *
 * Returns true if at least one water pixel moves.
 */
bool updateWaterFlow()
{
    bool waterMoved = false;

    for (uint8_t i = 0;
         i < MAX_MOVES_PER_STEP;
         ++i)
    {
        if (!moveOneWaterPixel()) {
            break;
        }

        waterMoved = true;
    }

    return waterMoved;
}

} // namespace

// ------------------------------------------------------------
// PUBLIC API
// ------------------------------------------------------------

void begin()
{
    initialized = true;
    reset(millis());
}

void reset(uint32_t nowMs)
{
    if (!initialized) {
        initialized = true;
    }

    // Read a safe MPU6050 snapshot.
    MPU6050::Data imu{};

    if (MPU6050::getData(imu) && imu.valid)
    {
        updateUpsideDownOrientation(imu, nowMs);
        updateGravityDirection(imu);
    }

    // Generate the initial 64-pixel water distribution.
    initializeWater();

    scanCursor = 0;
    lastFlowStepMs = nowMs;

    // Initial frame: 32 water pixels in each chamber.
    render();
}

void update(uint32_t nowMs)
{
    if (!initialized) {
        begin();
        return;
    }

    bool frameChanged = false;

    // Read one consistent MPU6050 snapshot for this update.
    MPU6050::Data imu{};

    if (MPU6050::getData(imu) && imu.valid)
    {
        // Detect automatic display orientation changes.
        if (updateUpsideDownOrientation(imu, nowMs)) {
            frameChanged = true;
        }

        // Update gravity continuously from the sensor.
        updateGravityDirection(imu);
    }

    // Run the water simulation at a controlled interval.
    if ((uint32_t)(nowMs - lastFlowStepMs) >= FLOW_STEP_MS)
    {
        lastFlowStepMs = nowMs;

        if (updateWaterFlow()) {
            frameChanged = true;
        }
    }

    // Render only when the display contents have changed.
    if (frameChanged) {
        render();
    }
}

} // namespace WaterAnimation

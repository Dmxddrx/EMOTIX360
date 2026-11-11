
#include "water_animation.h"

#include "water_geometry.h"
#include "max7219.h"
#include "mpu6050.h"

#include <Arduino.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>

namespace WaterAnimation {
namespace {

// ------------------------------------------------------------
// WATER SIMULATION CONFIGURATION
// ------------------------------------------------------------

constexpr uint8_t WATER_PARTICLE_COUNT = 64;

constexpr uint16_t NODE_COUNT =
    WaterGeometry::NODE_COUNT;

constexpr uint8_t MATRIX_WIDTH =
    WaterGeometry::MATRIX_WIDTH;

constexpr uint8_t MATRIX_HEIGHT =
    WaterGeometry::MATRIX_HEIGHT;

// ------------------------------------------------------------
// PHYSICS UPDATE TIMING
// ------------------------------------------------------------

// AnimationManager normally calls update() every 10ms.
// Fluid physics is calculated at 20ms intervals.
constexpr uint32_t FLOW_STEP_MS = 20;

// Maximum elapsed time accepted for one physics step.
constexpr float MAX_PHYSICS_DT = 0.08f;

// ------------------------------------------------------------
// GRAVITY ESTIMATION
// ------------------------------------------------------------

// Gyroscope integration is corrected using accelerometer data.
constexpr float ACCEL_CORRECTION_TAU = 0.25f;

// Low-pass filtering for the gravity projection magnitude.
constexpr float GRAVITY_MAGNITUDE_TAU = 0.12f;

// Sensor mounting:
//
// MPU Y+ points LEFT.
// MPU Z+ points DOWN.
// MPU X+ points towards the user's face.
//
// Logical screen:
//
// Positive X = RIGHT.
// Positive Y = DOWN.
//
// Gravity mapping:
//     Screen gravity X = +MPU AY.
//     Screen gravity Y = -MPU AZ.
//
// The signs can be calibrated independently.
constexpr float FLOW_X_SIGN = 1.0f;
constexpr float FLOW_Y_SIGN = 1.0f;

// Gyroscope rotation around the screen-normal axis.
// Adjust this sign if rotation response is reversed.
constexpr float GYRO_X_SIGN = 1.0f;

constexpr float RADIANS_PER_DEGREE =
    0.017453292519943295f;

// ------------------------------------------------------------
// LIQUID PHYSICS
// ------------------------------------------------------------

// Gravitational acceleration applied to each particle.
// Units are approximate simulation pitches/squared per g.
constexpr float FLUID_ACCELERATION = 20.0f;

// Velocity damping.
// Larger values make the water settle faster.
constexpr float FLUID_DAMPING = 1.8f;

// Maximum particle velocity in physical simulation units.
constexpr float MAX_PARTICLE_SPEED = 5.5f;

// Very small velocities are treated as stationary.
constexpr float MIN_PARTICLE_SPEED = 0.04f;

// Movement-credit accumulation.
// Higher values make particles move between LEDs faster.
constexpr float FLOW_DISTANCE_GAIN = 1.0f;

// A particle must accumulate this fraction of the
// neighbour distance before its LED position changes.
constexpr float MOVE_DISTANCE_THRESHOLD = 0.85f;

// Maximum accumulated movement credit.
constexpr float MAX_MOVEMENT_CREDIT = 1.75f;

// Minimum alignment between particle velocity and
// the direction to a free neighbour.
constexpr float MIN_FLOW_ALIGNMENT = 0.08f;

// When blocked, remove part of the velocity pointing
// directly into the occupied neighbouring particle.
// Tangential movement is retained to encourage sliding.
constexpr float COLLISION_NORMAL_DAMPING = 0.75f;

// Reduce movement credit when a particle is blocked.
constexpr float BLOCKED_CREDIT_LIMIT = 0.25f;

// ------------------------------------------------------------
// WATER PARTICLE DATA
// ------------------------------------------------------------

/*
 * WATER PARTICLE
 *
 * Every particle represents one illuminated MAX7219 LED.
 *
 * nodeID:
 *     Physical LED node occupied by this particle.
 *
 * velocityX / velocityY:
 *     Persistent liquid velocity in physical coordinates.
 *
 * movementCredit:
 *     Accumulated physical travel distance before the
 *     particle moves to its next neighbouring LED.
 */
struct WaterParticle
{
    uint8_t nodeID;

    float velocityX;
    float velocityY;

    float movementCredit;
};

// 64 individual water particles.
WaterParticle particles[WATER_PARTICLE_COUNT] = {};

// ------------------------------------------------------------
// WATER OCCUPANCY FRAMEBUFFER
// ------------------------------------------------------------

/*
 * LOGICAL WATER FRAMEBUFFER
 *
 * This uses the existing MAX7219 mapping:
 *
 *     water[y][0..7]   = Upper slave.
 *     water[y][8..15]  = Lower master.
 *
 * true:
 *     An illuminated LED contains water.
 *
 * false:
 *     The physical LED position is empty.
 *
 * The number of true cells must always remain 64.
 */
bool water[MATRIX_HEIGHT][MATRIX_WIDTH] = {};

// ------------------------------------------------------------
// GRAVITY ESTIMATOR STATE
// ------------------------------------------------------------

// Gravity angle convention:
//
//     0 radians       = downward.
//     +PI/2 radians   = right.
//     -PI/2 radians   = left.
//     PI radians      = upward.

float gravityAngleRad = 0.0f;

float gravityMagnitude = 1.0f;

float gravityX = 0.0f;
float gravityY = 1.0f;

bool gravityInitialized = false;

bool imuTimestampInitialized = false;

uint32_t lastImuTimestampMs = 0;

// ------------------------------------------------------------
// SIMULATION STATE
// ------------------------------------------------------------

bool initialized = false;

uint32_t lastPhysicsMs = 0;

// ------------------------------------------------------------
// GENERAL HELPERS
// ------------------------------------------------------------

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

/*
 * NORMALIZE ANGLE
 *
 * Keep the angle within -PI..+PI.
 */
float normalizeAngle(float angle)
{
    return atan2f(sinf(angle), cosf(angle));
}

// ------------------------------------------------------------
// OCCUPANCY HELPERS
// ------------------------------------------------------------

/*
 * SET NODE OCCUPANCY
 *
 * Update the logical water framebuffer using a physical
 * geometry node ID.
 */
bool setNodeOccupancy(uint8_t nodeId, bool occupied)
{
    uint8_t x;
    uint8_t y;

    if (!WaterGeometry::getCoordinates(nodeId, x, y)) {
        return false;
    }

    water[y][x] = occupied;

    return true;
}

/*
 * CHECK NODE OCCUPANCY
 *
 * Returns true if the physical LED already contains water.
 */
bool isNodeOccupied(uint8_t nodeId)
{
    uint8_t x;
    uint8_t y;

    if (!WaterGeometry::getCoordinates(nodeId, x, y)) {
        return true;
    }

    return water[y][x];
}

/*
 * COUNT WATER CELLS
 *
 * Calculate the number of occupied positions in water[][].
 */
uint8_t countOccupancy()
{
    uint16_t count = 0;

    for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y)
    {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x)
        {
            if (water[y][x]) {
                ++count;
            }
        }
    }

    return (uint8_t)count;
}

// ------------------------------------------------------------
// GRAVITY ESTIMATION
// ------------------------------------------------------------

/*
 * UPDATE GRAVITY ESTIMATE
 *
 * MPU6050 mounting:
 *
 *     MPU Y+ points LEFT.
 *     MPU Z+ points DOWN.
 *     MPU X+ points towards the user.
 *
 * Physical gravity projection onto the display plane:
 *
 *     Gravity X = +AY.
 *     Gravity Y = -AZ.
 *
 * This mapping accounts for:
 *
 *     - The physical MPU axis directions.
 *     - The accelerometer's stationary specific-force sign.
 *     - The logical matrix coordinate directions.
 *
 * GYROSCOPE:
 *     Integrates rotation about the screen-normal MPU X axis.
 *
 * ACCELEROMETER:
 *     Corrects the estimated gravity angle over time.
 *
 * This complementary filter allows the liquid to preserve
 * its momentum during a sudden flip without instantly
 * reversing its direction.
 *
 * Returns true when a new MPU6050 sample is processed.
 */
bool updateGravityEstimate()
{
    MPU6050::Data imu{};

    if (!MPU6050::getData(imu) || !imu.valid) {
        return false;
    }

    // Process every MPU6050 sample only once.
    if (imuTimestampInitialized &&
        imu.timestampMs == lastImuTimestampMs)
    {
        return false;
    }

    // Determine the time between consecutive sensor samples.
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

    // --------------------------------------------------------
    // ACCELEROMETER PROJECTION ONTO DISPLAY PLANE
    // --------------------------------------------------------

    const float accelGravityX =
        imu.ayG * FLOW_X_SIGN;

    const float accelGravityY =
        -imu.azG * FLOW_Y_SIGN;

    const float measuredGravityMagnitude =
        sqrtf(
            (accelGravityX * accelGravityX) +
            (accelGravityY * accelGravityY)
        );

    // Total acceleration magnitude.
    const float totalAcceleration =
        sqrtf(
            (imu.axG * imu.axG) +
            (imu.ayG * imu.ayG) +
            (imu.azG * imu.azG)
        );

    // --------------------------------------------------------
    // ACCELEROMETER CONFIDENCE
    // --------------------------------------------------------

    /*
     * During strong linear acceleration, the accelerometer
     * does not measure gravity alone.
     *
     * Gyroscope integration should dominate in that situation.
     */
    const float accelerationError =
        fabsf(totalAcceleration - 1.0f);

    const float accelConfidence =
        clampFloat(
            1.0f - (accelerationError / 0.50f),
            0.0f,
            1.0f
        );

    // --------------------------------------------------------
    // INITIALIZE GRAVITY ANGLE
    // --------------------------------------------------------

    if (!gravityInitialized)
    {
        if (measuredGravityMagnitude > 0.05f)
        {
            gravityAngleRad =
                atan2f(
                    accelGravityX,
                    accelGravityY
                );

            gravityMagnitude =
                measuredGravityMagnitude;
        }
        else
        {
            // If gravity is almost perpendicular to the screen,
            // use the default lower-screen direction.
            gravityAngleRad = 0.0f;
            gravityMagnitude = 0.0f;
        }

        gravityInitialized = true;
    }
    else
    {
        // ----------------------------------------------------
        // GYROSCOPE ANGLE INTEGRATION
        // ----------------------------------------------------

        const float gyroRateRad =
            imu.gxDps *
            RADIANS_PER_DEGREE *
            GYRO_X_SIGN;

        // Preserve the existing liquid orientation using
        // the new angular velocity.
        gravityAngleRad += gyroRateRad * dt;

        gravityAngleRad =
            normalizeAngle(gravityAngleRad);

        // ----------------------------------------------------
        // ACCELEROMETER ANGLE CORRECTION
        // ----------------------------------------------------

        if (measuredGravityMagnitude > 0.05f &&
            accelConfidence > 0.01f)
        {
            const float accelAngle =
                atan2f(
                    accelGravityX,
                    accelGravityY
                );

            // Calculate the shortest angular error.
            const float angleError =
                atan2f(
                    sinf(accelAngle - gravityAngleRad),
                    cosf(accelAngle - gravityAngleRad)
                );

            const float correction =
                (1.0f - expf(-dt / ACCEL_CORRECTION_TAU)) *
                accelConfidence;

            gravityAngleRad +=
                angleError * correction;

            gravityAngleRad =
                normalizeAngle(gravityAngleRad);
        }

        // ----------------------------------------------------
        // GRAVITY PROJECTION MAGNITUDE FILTER
        // ----------------------------------------------------

        // Only trust the magnitude measurement when the
        // accelerometer is reasonably close to 1g.
        if (accelConfidence > 0.1f)
        {
            const float magnitudeAlpha =
                1.0f - expf(-dt / GRAVITY_MAGNITUDE_TAU);

            gravityMagnitude +=
                magnitudeAlpha * accelConfidence *
                (measuredGravityMagnitude - gravityMagnitude);
        }
    }

    gravityMagnitude =
        clampFloat(gravityMagnitude, 0.0f, 1.5f);

    // --------------------------------------------------------
    // FINAL PHYSICAL GRAVITY VECTOR
    // --------------------------------------------------------

    gravityX =
        sinf(gravityAngleRad) * gravityMagnitude;

    gravityY =
        cosf(gravityAngleRad) * gravityMagnitude;

    return true;
}

// ------------------------------------------------------------
// FLUID VELOCITY
// ------------------------------------------------------------

/*
 * UPDATE PARTICLE VELOCITY
 *
 * Each water particle maintains its own velocity.
 *
 * Gravity accelerates the particle.
 * Damping reduces its velocity over time.
 *
 * When the device is suddenly rotated:
 *
 *     - Gravity direction changes.
 *     - Previous velocity is retained.
 *     - The particle gradually slows and changes direction.
 *
 * This is the main source of the liquid inertia effect.
 */
void updateParticleVelocities(float dt)
{
    const float dampingFactor =
        expf(-FLUID_DAMPING * dt);

    for (uint8_t i = 0;
         i < WATER_PARTICLE_COUNT;
         ++i)
    {
        WaterParticle &particle = particles[i];

        // Apply gravitational acceleration.
        particle.velocityX +=
            gravityX * FLUID_ACCELERATION * dt;

        particle.velocityY +=
            gravityY * FLUID_ACCELERATION * dt;

        // Apply fluid damping.
        particle.velocityX *= dampingFactor;
        particle.velocityY *= dampingFactor;

        // Limit maximum velocity.
        const float speed =
            sqrtf(
                (particle.velocityX * particle.velocityX) +
                (particle.velocityY * particle.velocityY)
            );

        if (speed > MAX_PARTICLE_SPEED)
        {
            const float scale =
                MAX_PARTICLE_SPEED / speed;

            particle.velocityX *= scale;
            particle.velocityY *= scale;
        }
    }
}

// ------------------------------------------------------------
// SHUFFLE PARTICLE UPDATE ORDER
// ------------------------------------------------------------

/*
 * BUILD RANDOM PARTICLE UPDATE ORDER
 *
 * Randomized processing reduces directional bias.
 *
 * Every particle is still updated exactly once per step.
 */
void buildRandomParticleOrder(uint8_t *order)
{
    for (uint8_t i = 0;
         i < WATER_PARTICLE_COUNT;
         ++i)
    {
        order[i] = i;
    }

    // Fisher-Yates shuffle.
    for (int16_t i =
             (int16_t)WATER_PARTICLE_COUNT - 1;
         i > 0;
         --i)
    {
        const uint8_t j =
            (uint8_t)(
                esp_random() % (uint32_t)(i + 1)
            );

        const uint8_t temp =
            order[i];

        order[i] = order[j];
        order[j] = temp;
    }
}

// ------------------------------------------------------------
// PARTICLE COLLISION RESPONSE
// ------------------------------------------------------------

/*
 * HANDLE BLOCKED PARTICLE
 *
 * When a particle attempts to move towards an occupied
 * neighbouring position:
 *
 *     - Remove part of the velocity normal to that neighbour.
 *     - Preserve the tangential component.
 *     - Reduce accumulated movement credit.
 *
 * This approximates particle-to-particle collision and
 * encourages water to slide around occupied positions.
 */
void resolveBlockedMovement(
    WaterParticle &particle,
    float normalX,
    float normalY
)
{
    const float normalVelocity =
        (particle.velocityX * normalX) +
        (particle.velocityY * normalY);

    // Only remove velocity directed into the obstacle.
    if (normalVelocity > 0.0f)
    {
        particle.velocityX -=
            COLLISION_NORMAL_DAMPING *
            normalVelocity *
            normalX;

        particle.velocityY -=
            COLLISION_NORMAL_DAMPING *
            normalVelocity *
            normalY;
    }

    // Avoid building a large movement burst while blocked.
    particle.movementCredit =
        fminf(
            particle.movementCredit,
            BLOCKED_CREDIT_LIMIT
        );
}

// ------------------------------------------------------------
// MOVE ONE WATER PARTICLE
// ------------------------------------------------------------

/*
 * MOVE PARTICLE THROUGH PHYSICAL NEIGHBOURS
 *
 * The physical neighbour graph is generated by WaterGeometry.
 *
 * The movement algorithm never assumes that two adjacent
 * logical framebuffer coordinates are physical neighbours.
 *
 * A movement is allowed only when:
 *
 * 1. A physical neighbour connection exists.
 * 2. The destination LED is empty.
 * 3. The destination is aligned with particle velocity.
 * 4. Sufficient physical travel has accumulated.
 *
 * Returns true when the particle moves to a new LED.
 */
bool moveParticle(
    WaterParticle &particle,
    float dt
)
{
    const WaterGeometry::Node *currentNode =
        WaterGeometry::getNode(particle.nodeID);

    if (currentNode == nullptr) {
        return false;
    }

    const float speed =
        sqrtf(
            (particle.velocityX * particle.velocityX) +
            (particle.velocityY * particle.velocityY)
        );

    if (speed < MIN_PARTICLE_SPEED) {
        return false;
    }

    // --------------------------------------------------------
    // FIND BEST AVAILABLE PHYSICAL NEIGHBOUR
    // --------------------------------------------------------

    int16_t bestFreeNode = -1;

    float bestFreeScore = MIN_FLOW_ALIGNMENT;

    float bestFreeDistance = 0.0f;

    // Track the best occupied neighbour for collision handling.
    float bestBlockedScore = MIN_FLOW_ALIGNMENT;

    float blockedNormalX = 0.0f;
    float blockedNormalY = 0.0f;

    for (uint8_t i = 0;
         i < currentNode->neighbourCount;
         ++i)
    {
        const uint8_t neighbourID =
            currentNode->neighbours[i];

        const WaterGeometry::Node *neighbourNode =
            WaterGeometry::getNode(neighbourID);

        if (neighbourNode == nullptr) {
            continue;
        }

        const float distance =
            currentNode->neighbourDistances[i];

        if (distance <= 0.001f) {
            continue;
        }

        // Calculate physical movement direction.
        const float directionX =
            (neighbourNode->x - currentNode->x) / distance;

        const float directionY =
            (neighbourNode->y - currentNode->y) / distance;

        // Alignment between particle velocity and neighbour direction.
        const float alignment =
            (
                (particle.velocityX * directionX) +
                (particle.velocityY * directionY)
            ) / speed;

        // ----------------------------------------------------
        // FREE NEIGHBOUR
        // ----------------------------------------------------

        if (!isNodeOccupied(neighbourID))
        {
            // Small random variation breaks equal-score ties.
            const float randomTieBreak =
                (float)(esp_random() % 1000) * 0.000001f;

            const float score =
                alignment + randomTieBreak;

            if (score > bestFreeScore)
            {
                bestFreeScore = score;

                bestFreeNode = neighbourID;

                bestFreeDistance = distance;
            }
        }
        else
        {
            // ------------------------------------------------
            // OCCUPIED NEIGHBOUR
            // ------------------------------------------------

            if (alignment > bestBlockedScore)
            {
                bestBlockedScore = alignment;

                blockedNormalX = directionX;
                blockedNormalY = directionY;
            }
        }
    }

    // --------------------------------------------------------
    // NO FREE NEIGHBOUR
    // --------------------------------------------------------

    if (bestFreeNode < 0)
    {
        if (bestBlockedScore > MIN_FLOW_ALIGNMENT)
        {
            // Preserve tangential flow around the obstacle.
            resolveBlockedMovement(
                particle,
                blockedNormalX,
                blockedNormalY
            );
        }
        else
        {
            // No useful movement direction exists.
            particle.velocityX *= 0.90f;
            particle.velocityY *= 0.90f;

            particle.movementCredit =
                fminf(
                    particle.movementCredit,
                    BLOCKED_CREDIT_LIMIT
                );
        }

        return false;
    }

    // --------------------------------------------------------
    // ACCUMULATE PHYSICAL TRAVEL DISTANCE
    // --------------------------------------------------------

    particle.movementCredit +=
        speed * dt * FLOW_DISTANCE_GAIN;

    particle.movementCredit =
        clampFloat(
            particle.movementCredit,
            0.0f,
            MAX_MOVEMENT_CREDIT
        );

    const float requiredDistance =
        bestFreeDistance * MOVE_DISTANCE_THRESHOLD;

    // Do not move until enough physical travel is accumulated.
    if (particle.movementCredit < requiredDistance) {
        return false;
    }

    // Check that the target is still empty.
    // Another particle may have occupied it earlier in this step.
    if (isNodeOccupied((uint8_t)bestFreeNode)) {
        particle.movementCredit =
            BLOCKED_CREDIT_LIMIT;

        return false;
    }

    // --------------------------------------------------------
    // MOVE PARTICLE
    // --------------------------------------------------------

    const uint8_t oldNode =
        particle.nodeID;

    const uint8_t newNode =
        (uint8_t)bestFreeNode;

    // Remove water from the old position.
    if (!setNodeOccupancy(oldNode, false)) {
        return false;
    }

    // Add water to the new position.
    if (!setNodeOccupancy(newNode, true))
    {
        // Restore the old position if the destination update fails.
        setNodeOccupancy(oldNode, true);

        return false;
    }

    // Update particle position.
    particle.nodeID = newNode;

    // Consume the travelled distance.
    particle.movementCredit -= requiredDistance;

    return true;
}

// ------------------------------------------------------------
// PARTICLE STATE VALIDATION
// ------------------------------------------------------------

/*
 * REBUILD OCCUPANCY
 *
 * Recreate the water framebuffer using the particle list.
 *
 * This also detects accidental duplicate particle positions.
 */
bool rebuildOccupancy()
{
    memset(water, 0, sizeof(water));

    for (uint8_t i = 0;
         i < WATER_PARTICLE_COUNT;
         ++i)
    {
        const uint8_t nodeID =
            particles[i].nodeID;

        if (nodeID >= NODE_COUNT) {
            return false;
        }

        if (isNodeOccupied(nodeID)) {
            return false;
        }

        if (!setNodeOccupancy(nodeID, true)) {
            return false;
        }
    }

    return countOccupancy() == WATER_PARTICLE_COUNT;
}

/*
 * VALIDATE WATER CONSERVATION
 *
 * Every particle must occupy a unique physical node.
 * Occupancy must contain exactly 64 water pixels.
 */
bool validateWaterState()
{
    bool visited[NODE_COUNT] = {};

    for (uint8_t i = 0;
         i < WATER_PARTICLE_COUNT;
         ++i)
    {
        const uint8_t nodeID =
            particles[i].nodeID;

        if (nodeID >= NODE_COUNT || visited[nodeID]) {
            Serial.println(
                "[WATER] ERROR: Duplicate or invalid particle node"
            );

            return false;
        }

        visited[nodeID] = true;
    }

    if (countOccupancy() != WATER_PARTICLE_COUNT)
    {
        Serial.printf(
            "[WATER] ERROR: Incorrect water count: %u\n",
            countOccupancy()
        );

        return false;
    }

    return true;
}

// ------------------------------------------------------------
// INITIAL WATER DISTRIBUTION
// ------------------------------------------------------------

/*
 * INITIALIZE WATER PARTICLES
 *
 * The initial water distribution is determined from the
 * current physical gravity direction.
 *
 * The 128 physical LED nodes are sorted by their projection
 * onto gravity.
 *
 * The 64 nodes furthest in the gravity direction are occupied.
 *
 * Upright:
 *     Water starts in the lower master chamber.
 *
 * Upside down:
 *     Water starts in the upper slave chamber.
 *
 * Tilted:
 *     Water starts towards the physically lower side.
 *
 * IMPORTANT:
 *
 *     HOURGLASS_PRIORITY is NOT used to rank or select water
 *     movement positions.
 *
 *     Physical node coordinates determine the initial fill.
 */
bool initializeWater()
{
    struct SeedNode {
        uint8_t nodeID;
        float gravityProjection;
    };

    SeedNode sortedNodes[NODE_COUNT] = {};

    float directionX = gravityX;
    float directionY = gravityY;

    float magnitude =
        sqrtf(
            (directionX * directionX) +
            (directionY * directionY)
        );

    // Use the lower-screen direction when gravity is
    // nearly perpendicular to the display.
    if (magnitude < 0.05f)
    {
        directionX = 0.0f;
        directionY = 1.0f;
    }
    else
    {
        directionX /= magnitude;
        directionY /= magnitude;
    }

    // --------------------------------------------------------
    // CALCULATE GRAVITY PROJECTION FOR ALL NODES
    // --------------------------------------------------------

    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        const WaterGeometry::Node *node =
            WaterGeometry::getNode((uint8_t)id);

        if (node == nullptr) {
            return false;
        }

        sortedNodes[id].nodeID = (uint8_t)id;

        sortedNodes[id].gravityProjection =
            (node->x * directionX) +
            (node->y * directionY);
    }

    // --------------------------------------------------------
    // SORT FROM PHYSICALLY LOWEST TO HIGHEST
    // --------------------------------------------------------

    // Insertion sort is sufficient for only 128 nodes.
    for (uint16_t i = 1; i < NODE_COUNT; ++i)
    {
        const SeedNode key = sortedNodes[i];

        int16_t j = (int16_t)i - 1;

        while (j >= 0 &&
               sortedNodes[j].gravityProjection <
               key.gravityProjection)
        {
            sortedNodes[j + 1] = sortedNodes[j];

            --j;
        }

        sortedNodes[j + 1] = key;
    }

    // --------------------------------------------------------
    // RESET OCCUPANCY
    // --------------------------------------------------------

    memset(water, 0, sizeof(water));

    // --------------------------------------------------------
    // PLACE EXACTLY 64 WATER PARTICLES
    // --------------------------------------------------------

    for (uint8_t i = 0;
         i < WATER_PARTICLE_COUNT;
         ++i)
    {
        WaterParticle &particle =
            particles[i];

        particle.nodeID =
            sortedNodes[i].nodeID;

        particle.velocityX = 0.0f;
        particle.velocityY = 0.0f;

        particle.movementCredit = 0.0f;

        if (!setNodeOccupancy(particle.nodeID, true)) {
            return false;
        }
    }

    // Validate the initial distribution.
    return validateWaterState();
}

// ------------------------------------------------------------
// FRAMEBUFFER RENDERING
// ------------------------------------------------------------

/*
 * RENDER WATER FRAME
 *
 * The logical framebuffer is identical to SandClock:
 *
 *     X = 0..7    Upper slave.
 *     X = 8..15   Lower master.
 *
 * No extra rotation is applied.
 *
 * All physical module mounting transformations were already
 * included when WaterGeometry generated the neighbour graph.
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
                MAX7219_SetPixel(x, y, 1);
            }
        }
    }

    MAX7219_Update();
}

// ------------------------------------------------------------
// COMPLETE PHYSICS STEP
// ------------------------------------------------------------

/*
 * UPDATE WATER PHYSICS
 *
 * Physics sequence:
 *
 * 1. Update each particle's velocity using gravity.
 * 2. Apply fluid damping.
 * 3. Randomize the particle processing order.
 * 4. Select physically connected neighbouring nodes.
 * 5. Move particles into free nodes when sufficient distance
 *    has accumulated.
 * 6. Resolve blocked movement using collision damping.
 * 7. Validate water conservation.
 *
 * Each particle is processed only once per physics step.
 *
 * Returns true when at least one particle changes position.
 */
bool updatePhysics(float dt)
{
    // Update velocity before moving any particles.
    updateParticleVelocities(dt);

    // Build a randomized processing order.
    uint8_t particleOrder[WATER_PARTICLE_COUNT] = {};

    buildRandomParticleOrder(particleOrder);

    bool waterMoved = false;

    // Each particle is allowed at most one move per step.
    for (uint8_t i = 0;
         i < WATER_PARTICLE_COUNT;
         ++i)
    {
        const uint8_t particleIndex =
            particleOrder[i];

        WaterParticle &particle =
            particles[particleIndex];

        if (moveParticle(particle, dt)) {
            waterMoved = true;
        }
    }

    // Protect the total water volume.
    if (!validateWaterState())
    {
        Serial.println(
            "[WATER] Rebuilding occupancy framebuffer"
        );

        if (!rebuildOccupancy())
        {
            Serial.println(
                "[WATER] CRITICAL: Particle state invalid"
            );
        }

        // Update the display after recovering occupancy.
        return true;
    }

    return waterMoved;
}

// ------------------------------------------------------------
// RESET PHYSICS STATE
// ------------------------------------------------------------

void resetPhysicsState(uint32_t nowMs)
{
    gravityAngleRad = 0.0f;

    gravityMagnitude = 1.0f;

    gravityX = 0.0f;
    gravityY = 1.0f;

    gravityInitialized = false;

    imuTimestampInitialized = false;
    lastImuTimestampMs = 0;

    lastPhysicsMs = nowMs;
}

// ------------------------------------------------------------
// PUBLIC API
// ------------------------------------------------------------

} // namespace

// ------------------------------------------------------------
// INITIALIZATION
// ------------------------------------------------------------

void begin()
{
    // Build the physical 128-LED geometry.
    if (!WaterGeometry::begin())
    {
        initialized = false;

        Serial.println(
            "[WATER] Physical geometry initialization failed"
        );

        return;
    }

    initialized = true;

    // Start with a fresh water distribution.
    reset(millis());

    Serial.println("[WATER] Water simulation initialized");
}

// ------------------------------------------------------------
// RESET
// ------------------------------------------------------------

void reset(uint32_t nowMs)
{
    // Ensure the physical geometry is available.
    if (!WaterGeometry::isReady())
    {
        if (!WaterGeometry::begin())
        {
            initialized = false;
            return;
        }
    }

    initialized = true;

    // Reset all physics state.
    resetPhysicsState(nowMs);

    // Read the latest sensor sample before placing water.
    // This determines which chamber is physically lower.
    updateGravityEstimate();

    // Place exactly 64 particles according to current gravity.
    if (!initializeWater())
    {
        initialized = false;

        Serial.println(
            "[WATER] Initial water distribution failed"
        );

        return;
    }

    lastPhysicsMs = nowMs;

    // Display the initial liquid distribution.
    render();

    Serial.printf(
        "[WATER] Initial water particles: %u\n",
        getWaterCount()
    );
}

// ------------------------------------------------------------
// PERIODIC UPDATE
// ------------------------------------------------------------

void update(uint32_t nowMs)
{
    if (!initialized)
    {
        begin();

        if (!initialized) {
            return;
        }
    }

    // Update the gravity estimator whenever a new MPU6050
    // data sample becomes available.
    //
    // It does not rotate or redraw the physical matrix.
    updateGravityEstimate();

    // Calculate elapsed physics time.
    const uint32_t elapsedMs =
        (uint32_t)(nowMs - lastPhysicsMs);

    if (elapsedMs < FLOW_STEP_MS) {
        return;
    }

    lastPhysicsMs = nowMs;

    float dt = elapsedMs * 0.001f;

    // Prevent unstable movement after long scheduling delays.
    dt = clampFloat(dt, 0.001f, MAX_PHYSICS_DT);

    // Run one complete liquid physics step.
    if (updatePhysics(dt))
    {
        render();
    }
}

// ------------------------------------------------------------
// DIAGNOSTICS
// ------------------------------------------------------------

bool isReady()
{
    return initialized && WaterGeometry::isReady();
}

uint8_t getWaterCount()
{
    return countOccupancy();
}

} // namespace WaterAnimation

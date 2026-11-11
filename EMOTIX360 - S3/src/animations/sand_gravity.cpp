
#include "sand_gravity.h"

#include "water_geometry.h"
#include "max7219.h"
#include "mpu6050.h"

#include <Arduino.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>

namespace SandGravity {
namespace {

// ------------------------------------------------------------
// SIMULATION CONFIGURATION
// ------------------------------------------------------------

constexpr uint8_t SAND_TOTAL = 64;

constexpr uint8_t MATRIX_WIDTH =
    WaterGeometry::MATRIX_WIDTH;

constexpr uint8_t MATRIX_HEIGHT =
    WaterGeometry::MATRIX_HEIGHT;

constexpr uint16_t NODE_COUNT =
    WaterGeometry::NODE_COUNT;

// ------------------------------------------------------------
// PHYSICS TIMING
// ------------------------------------------------------------

// AnimationManager calls update() approximately every 10ms.
constexpr uint32_t PHYSICS_STEP_MS = 25;

// Limit time steps after scheduling delays.
constexpr float MAX_PHYSICS_DT = 0.08f;

// ------------------------------------------------------------
// MPU6050 GRAVITY ESTIMATION
// ------------------------------------------------------------

// Use the same MPU6050 mounting and gravity mapping
// as WaterAnimation.

constexpr float ACCEL_CORRECTION_TAU = 0.25f;
constexpr float GRAVITY_MAGNITUDE_TAU = 0.12f;

constexpr float FLOW_X_SIGN = 1.0f;
constexpr float FLOW_Y_SIGN = 1.0f;
constexpr float GYRO_X_SIGN = 1.0f;

constexpr float RADIANS_PER_DEGREE =
    0.017453292519943295f;

// ------------------------------------------------------------
// SAND PHYSICS
// ------------------------------------------------------------

// Sand accelerates more slowly than water.
constexpr float SAND_ACCELERATION = 14.0f;

// Higher damping makes grains settle instead of flowing
// continuously like liquid.
constexpr float SAND_DAMPING = 3.4f;

// Maximum grain velocity.
constexpr float MAX_SAND_SPEED = 3.5f;

// Ignore very small residual movement.
constexpr float MIN_SAND_SPEED = 0.035f;

// Physical distance accumulated before changing LED position.
constexpr float SAND_MOVEMENT_GAIN = 0.80f;

constexpr float MOVE_DISTANCE_THRESHOLD = 0.72f;

// Minimum directional alignment required to select a neighbour.
constexpr float MIN_FLOW_ALIGNMENT = 0.12f;

// Collision response.
// Sand loses more energy during collisions than water.
constexpr float COLLISION_NORMAL_DAMPING = 0.90f;
constexpr float COLLISION_TANGENT_DAMPING = 0.82f;

constexpr float BLOCKED_CREDIT_LIMIT = 0.15f;

// Maximum accumulated movement credit.
constexpr float MAX_MOVEMENT_CREDIT = 1.5f;

// ------------------------------------------------------------
// SENSOR / GRAVITY STATE
// ------------------------------------------------------------

float gravityAngleRad = 0.0f;
float gravityMagnitude = 1.0f;

float gravityX = 0.0f;
float gravityY = 1.0f;

bool gravityInitialized = false;

bool imuTimestampInitialized = false;
uint32_t lastImuTimestampMs = 0;

// ------------------------------------------------------------
// SAND PARTICLE DATA
// ------------------------------------------------------------

/*
 * EACH SAND PARTICLE
 *
 * nodeID:
 *     Physical LED currently occupied by this grain.
 *
 * velocityX / velocityY:
 *     Persistent grain velocity in physical coordinates.
 *
 * movementCredit:
 *     Accumulated travel distance before the grain moves
 *     to another physical LED.
 */
struct SandParticle
{
    uint8_t nodeID;

    float velocityX;
    float velocityY;

    float movementCredit;
};

// Fixed 64-grain simulation.
SandParticle particles[SAND_TOTAL] = {};

// ------------------------------------------------------------
// SAND OCCUPANCY FRAMEBUFFER
// ------------------------------------------------------------

/*
 * Logical MAX7219 framebuffer:
 *
 *     X = 0..7    Upper slave.
 *     X = 8..15   Lower master.
 *
 *     Y = 0..7    Module-local row.
 *
 * true:
 *     Sand grain occupies this LED.
 *
 * false:
 *     Empty.
 */
bool sand[MATRIX_HEIGHT][MATRIX_WIDTH] = {};

// ------------------------------------------------------------
// SIMULATION STATE
// ------------------------------------------------------------

bool initialized = false;

uint32_t lastPhysicsMs = 0;

// The module containing the initial sand source.
// 0 = slave.
// 1 = master.
uint8_t sourceModule = 0;

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

float normalizeAngle(float angle)
{
    return atan2f(sinf(angle), cosf(angle));
}

// ------------------------------------------------------------
// OCCUPANCY HELPERS
// ------------------------------------------------------------

bool setNodeOccupancy(uint8_t nodeID, bool occupied)
{
    uint8_t x;
    uint8_t y;

    if (!WaterGeometry::getCoordinates(nodeID, x, y)) {
        return false;
    }

    sand[y][x] = occupied;

    return true;
}

bool isNodeOccupied(uint8_t nodeID)
{
    uint8_t x;
    uint8_t y;

    if (!WaterGeometry::getCoordinates(nodeID, x, y)) {
        return true;
    }

    return sand[y][x];
}

uint8_t countOccupancy()
{
    uint16_t count = 0;

    for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y)
    {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x)
        {
            if (sand[y][x]) {
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
 *     Y+ points LEFT.
 *     Z+ points DOWN.
 *     X+ points towards the user.
 *
 * Logical screen coordinates:
 *
 *     X+ = RIGHT.
 *     Y+ = DOWN.
 *
 * Gravity projection:
 *
 *     Gravity X = +AY.
 *     Gravity Y = -AZ.
 *
 * Gyroscope X integrates rotation around the screen normal.
 *
 * Accelerometer corrects gyroscope drift.
 *
 * Existing sand particles retain their velocity while
 * the estimated gravity direction changes.
 */
bool updateGravityEstimate()
{
    MPU6050::Data imu{};

    if (!MPU6050::getData(imu) || !imu.valid) {
        return false;
    }

    // Process each sensor sample only once.
    if (imuTimestampInitialized &&
        imu.timestampMs == lastImuTimestampMs)
    {
        return false;
    }

    // Determine sensor sampling interval.
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
    // ACCELEROMETER GRAVITY PROJECTION
    // --------------------------------------------------------

    const float accelGravityX =
        imu.ayG * FLOW_X_SIGN;

    const float accelGravityY =
        -imu.azG * FLOW_Y_SIGN;

    const float measuredGravityMagnitude =
        sqrtf(
            accelGravityX * accelGravityX +
            accelGravityY * accelGravityY
        );

    const float totalAcceleration =
        sqrtf(
            imu.axG * imu.axG +
            imu.ayG * imu.ayG +
            imu.azG * imu.azG
        );

    // --------------------------------------------------------
    // ACCELEROMETER CONFIDENCE
    // --------------------------------------------------------

    // During strong linear acceleration, rely more heavily
    // on gyroscope integration.
    const float accelerationError =
        fabsf(totalAcceleration - 1.0f);

    const float accelConfidence =
        clampFloat(
            1.0f - (accelerationError / 0.50f),
            0.0f,
            1.0f
        );

    // --------------------------------------------------------
    // INITIAL GRAVITY ANGLE
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
            gravityAngleRad = 0.0f;
            gravityMagnitude = 0.0f;
        }

        gravityInitialized = true;
    }
    else
    {
        // ----------------------------------------------------
        // GYROSCOPE INTEGRATION
        // ----------------------------------------------------

        const float gyroRateRad =
            imu.gxDps *
            RADIANS_PER_DEGREE *
            GYRO_X_SIGN;

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
        // GRAVITY MAGNITUDE FILTER
        // ----------------------------------------------------

        if (accelConfidence > 0.1f)
        {
            const float alpha =
                1.0f - expf(-dt / GRAVITY_MAGNITUDE_TAU);

            gravityMagnitude +=
                alpha * accelConfidence *
                (measuredGravityMagnitude - gravityMagnitude);
        }
    }

    gravityMagnitude =
        clampFloat(gravityMagnitude, 0.0f, 1.5f);

    // --------------------------------------------------------
    // FINAL GRAVITY VECTOR
    // --------------------------------------------------------

    gravityX =
        sinf(gravityAngleRad) * gravityMagnitude;

    gravityY =
        cosf(gravityAngleRad) * gravityMagnitude;

    return true;
}

// ------------------------------------------------------------
// SELECT INITIAL SOURCE CHAMBER
// ------------------------------------------------------------

/*
 * SELECT UPHILL MODULE
 *
 * Initially, all 64 sand particles occupy one complete module.
 *
 * The source chamber is selected opposite to gravity.
 *
 * Upright:
 *     Slave module is the source.
 *
 * Upside down:
 *     Master module is the source.
 *
 * Sideways:
 *     Retain the previous source module to prevent
 *     arbitrary changes near the horizontal position.
 */
void selectSourceModule()
{
    float averageProjection[2] = {0.0f, 0.0f};

    uint16_t nodeCount[2] = {0, 0};

    float directionX = gravityX;
    float directionY = gravityY;

    float magnitude =
        sqrtf(
            directionX * directionX +
            directionY * directionY
        );

    if (magnitude < 0.05f)
    {
        // Default upright direction.
        directionX = 0.0f;
        directionY = 1.0f;
    }
    else
    {
        directionX /= magnitude;
        directionY /= magnitude;
    }

    // Calculate the average physical gravity projection
    // for both modules.
    for (uint16_t id = 0; id < NODE_COUNT; ++id)
    {
        const WaterGeometry::Node *node =
            WaterGeometry::getNode((uint8_t)id);

        if (node == nullptr) {
            continue;
        }

        const uint8_t module =
            ((id % MATRIX_WIDTH) < 8) ? 0 : 1;

        averageProjection[module] +=
            node->x * directionX +
            node->y * directionY;

        ++nodeCount[module];
    }

    if (nodeCount[0] == 0 || nodeCount[1] == 0) {
        sourceModule = 0;
        return;
    }

    averageProjection[0] /= nodeCount[0];
    averageProjection[1] /= nodeCount[1];

    // The uphill module has the lower gravity projection.
    const float difference =
        averageProjection[0] - averageProjection[1];

    // When both modules have approximately equal height,
    // keep the current source chamber.
    if (fabsf(difference) < 0.25f) {
        return;
    }

    sourceModule =
        (difference < 0.0f) ? 0 : 1;
}

// ------------------------------------------------------------
// PARTICLE INITIALIZATION
// ------------------------------------------------------------

/*
 * INITIALIZE SAND PARTICLES
 *
 * All 64 grains are placed in the uphill module.
 *
 * Every LED of the selected source chamber starts occupied.
 *
 * All particle velocities and movement credits start at zero.
 *
 * This recreates the traditional hourglass starting condition
 * without depending on the old random priority sequence.
 */
bool initializeSand()
{
    memset(sand, 0, sizeof(sand));
    memset(particles, 0, sizeof(particles));

    uint8_t particleIndex = 0;

    const uint8_t startX =
        (sourceModule == 0) ? 0 : 8;

    for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y)
    {
        for (uint8_t localX = 0;
             localX < 8;
             ++localX)
        {
            const uint8_t x =
                (uint8_t)(startX + localX);

            const uint8_t nodeID =
                WaterGeometry::getNodeId(x, y);

            if (nodeID == 0xFF ||
                particleIndex >= SAND_TOTAL)
            {
                return false;
            }

            SandParticle &particle =
                particles[particleIndex];

            particle.nodeID = nodeID;

            particle.velocityX = 0.0f;
            particle.velocityY = 0.0f;

            particle.movementCredit = 0.0f;

            if (!setNodeOccupancy(nodeID, true)) {
                return false;
            }

            ++particleIndex;
        }
    }

    return particleIndex == SAND_TOTAL &&
           countOccupancy() == SAND_TOTAL;
}

// ------------------------------------------------------------
// PARTICLE VELOCITY UPDATE
// ------------------------------------------------------------

/*
 * UPDATE SAND VELOCITY
 *
 * Each grain has an independent velocity.
 *
 * Gravity accelerates the grain.
 * Damping removes kinetic energy over time.
 *
 * Unlike the previous priority-based SandClock:
 *
 *     - Grains are not assigned fixed destinations.
 *     - Their movement follows current gravity.
 *     - They retain momentum during sudden rotations.
 *     - Collisions reduce movement.
 *
 * This produces an orientation-responsive granular simulation.
 */
void updateParticleVelocities(float dt)
{
    const float dampingFactor =
        expf(-SAND_DAMPING * dt);

    for (uint8_t i = 0; i < SAND_TOTAL; ++i)
    {
        SandParticle &particle = particles[i];

        // Apply gravity.
        particle.velocityX +=
            gravityX * SAND_ACCELERATION * dt;

        particle.velocityY +=
            gravityY * SAND_ACCELERATION * dt;

        // Apply granular damping.
        particle.velocityX *= dampingFactor;
        particle.velocityY *= dampingFactor;

        // Limit particle speed.
        const float speed =
            sqrtf(
                particle.velocityX * particle.velocityX +
                particle.velocityY * particle.velocityY
            );

        if (speed > MAX_SAND_SPEED)
        {
            const float scale =
                MAX_SAND_SPEED / speed;

            particle.velocityX *= scale;
            particle.velocityY *= scale;
        }
    }
}

// ------------------------------------------------------------
// PARTICLE UPDATE ORDER
// ------------------------------------------------------------

/*
 * SHUFFLE PARTICLE ORDER
 *
 * Process grains in randomized order.
 *
 * This reduces the tendency for a particular array index
 * or physical location to receive movement priority.
 */
void buildParticleOrder(uint8_t *order)
{
    for (uint8_t i = 0; i < SAND_TOTAL; ++i) {
        order[i] = i;
    }

    // Fisher-Yates shuffle.
    for (int16_t i = (int16_t)SAND_TOTAL - 1;
         i > 0;
         --i)
    {
        const uint8_t j =
            (uint8_t)(esp_random() % (uint32_t)(i + 1));

        const uint8_t temp = order[i];

        order[i] = order[j];
        order[j] = temp;
    }
}

// ------------------------------------------------------------
// SAND COLLISION RESPONSE
// ------------------------------------------------------------

/*
 * RESOLVE GRAIN COLLISION
 *
 * If a grain attempts to move towards an occupied neighbour:
 *
 *     - Reduce velocity towards the obstacle.
 *     - Reduce tangential velocity using granular friction.
 *     - Reduce accumulated movement credit.
 *
 * This is intentionally more damped than WaterAnimation.
 */
void resolveCollision(
    SandParticle &particle,
    float normalX,
    float normalY
)
{
    const float normalVelocity =
        particle.velocityX * normalX +
        particle.velocityY * normalY;

    // Remove movement directed into the occupied neighbour.
    if (normalVelocity > 0.0f)
    {
        particle.velocityX -=
            normalVelocity * normalX *
            COLLISION_NORMAL_DAMPING;

        particle.velocityY -=
            normalVelocity * normalY *
            COLLISION_NORMAL_DAMPING;
    }

    // Apply granular friction.
    particle.velocityX *= COLLISION_TANGENT_DAMPING;
    particle.velocityY *= COLLISION_TANGENT_DAMPING;

    // Remove most accumulated movement while blocked.
    particle.movementCredit =
        fminf(
            particle.movementCredit,
            BLOCKED_CREDIT_LIMIT
        );
}

// ------------------------------------------------------------
// MOVE ONE SAND PARTICLE
// ------------------------------------------------------------

/*
 * MOVE PARTICLE THROUGH PHYSICAL NEIGHBOURS
 *
 * The WaterGeometry neighbour graph supplies valid physical
 * connections between the rotated modules.
 *
 * A grain can move only when:
 *
 * 1. The destination is physically connected.
 * 2. The destination is empty.
 * 3. The destination broadly follows particle velocity.
 * 4. Sufficient travel distance has accumulated.
 *
 * A particle moves no more than one LED per physics update.
 *
 * Returns true when the grain moves.
 */
bool moveParticle(
    SandParticle &particle,
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
            particle.velocityX * particle.velocityX +
            particle.velocityY * particle.velocityY
        );

    if (speed < MIN_SAND_SPEED) {
        return false;
    }

    // --------------------------------------------------------
    // FIND BEST FREE NEIGHBOUR
    // --------------------------------------------------------

    int16_t bestFreeNode = -1;

    float bestFreeAlignment = MIN_FLOW_ALIGNMENT;

    float bestFreeDistance = 0.0f;

    // Remember the most relevant occupied neighbour for collision.
    float bestBlockedAlignment = MIN_FLOW_ALIGNMENT;

    float collisionNormalX = 0.0f;
    float collisionNormalY = 0.0f;

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

        const float directionX =
            (neighbourNode->x - currentNode->x) / distance;

        const float directionY =
            (neighbourNode->y - currentNode->y) / distance;

        // Compare particle velocity against the physical
        // direction of this neighbour connection.
        const float alignment =
            (
                particle.velocityX * directionX +
                particle.velocityY * directionY
            ) / speed;

        if (!isNodeOccupied(neighbourID))
        {
            // Slight randomness resolves equal-direction choices.
            const float tieBreak =
                (float)(esp_random() % 1000) * 0.000001f;

            const float score =
                alignment + tieBreak;

            if (score > bestFreeAlignment)
            {
                bestFreeAlignment = score;
                bestFreeNode = neighbourID;
                bestFreeDistance = distance;
            }
        }
        else
        {
            if (alignment > bestBlockedAlignment)
            {
                bestBlockedAlignment = alignment;

                collisionNormalX = directionX;
                collisionNormalY = directionY;
            }
        }
    }

    // --------------------------------------------------------
    // NO FREE MOVEMENT DIRECTION
    // --------------------------------------------------------

    if (bestFreeNode < 0)
    {
        if (bestBlockedAlignment > MIN_FLOW_ALIGNMENT)
        {
            resolveCollision(
                particle,
                collisionNormalX,
                collisionNormalY
            );
        }
        else
        {
            // Stronger settling when no usable neighbour exists.
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
    // ACCUMULATE TRAVEL DISTANCE
    // --------------------------------------------------------

    particle.movementCredit +=
        speed * dt * SAND_MOVEMENT_GAIN;

    particle.movementCredit =
        clampFloat(
            particle.movementCredit,
            0.0f,
            MAX_MOVEMENT_CREDIT
        );

    const float requiredDistance =
        bestFreeDistance * MOVE_DISTANCE_THRESHOLD;

    if (particle.movementCredit < requiredDistance) {
        return false;
    }

    // Verify that the target remains free.
    if (isNodeOccupied((uint8_t)bestFreeNode)) {
        particle.movementCredit =
            BLOCKED_CREDIT_LIMIT;

        return false;
    }

    // --------------------------------------------------------
    // TRANSFER ONE GRAIN
    // --------------------------------------------------------

    const uint8_t oldNode =
        particle.nodeID;

    const uint8_t newNode =
        (uint8_t)bestFreeNode;

    if (!setNodeOccupancy(oldNode, false)) {
        return false;
    }

    if (!setNodeOccupancy(newNode, true))
    {
        // Restore the original position if necessary.
        setNodeOccupancy(oldNode, true);
        return false;
    }

    particle.nodeID = newNode;

    // Consume the required travel distance.
    particle.movementCredit -= requiredDistance;

    return true;
}

// ------------------------------------------------------------
// PARTICLE STATE VALIDATION
// ------------------------------------------------------------

/*
 * VALIDATE SAND CONSERVATION
 *
 * Every particle must occupy one unique LED node.
 * Exactly 64 framebuffer cells must be occupied.
 */
bool validateSandState()
{
    bool occupiedNodes[NODE_COUNT] = {};

    for (uint8_t i = 0; i < SAND_TOTAL; ++i)
    {
        const uint8_t nodeID =
            particles[i].nodeID;

        if (nodeID >= NODE_COUNT ||
            occupiedNodes[nodeID])
        {
            Serial.println(
                "[SAND GRAVITY] ERROR: Invalid or duplicate particle"
            );

            return false;
        }

        occupiedNodes[nodeID] = true;
    }

    if (countOccupancy() != SAND_TOTAL)
    {
        Serial.printf(
            "[SAND GRAVITY] ERROR: Incorrect sand count: %u\n",
            countOccupancy()
        );

        return false;
    }

    return true;
}

/*
 * REBUILD OCCUPANCY
 *
 * Restore the framebuffer from the particle positions
 * after detecting an unexpected occupancy inconsistency.
 */
bool rebuildOccupancy()
{
    memset(sand, 0, sizeof(sand));

    for (uint8_t i = 0; i < SAND_TOTAL; ++i)
    {
        const uint8_t nodeID =
            particles[i].nodeID;

        if (nodeID >= NODE_COUNT ||
            isNodeOccupied(nodeID))
        {
            return false;
        }

        if (!setNodeOccupancy(nodeID, true)) {
            return false;
        }
    }

    return countOccupancy() == SAND_TOTAL;
}

// ------------------------------------------------------------
// FRAMEBUFFER RENDERING
// ------------------------------------------------------------

/*
 * RENDER SAND
 *
 * Uses the original working SandClock MAX7219 mapping.
 *
 * No extra display rotation is applied.
 *
 * Physical module orientation is handled by WaterGeometry.
 */
void render()
{
    MAX7219_Clear();

    for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y)
    {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x)
        {
            if (sand[y][x]) {
                MAX7219_SetPixel(x, y, 1);
            }
        }
    }

    MAX7219_Update();
}

// ------------------------------------------------------------
// PHYSICS STEP
// ------------------------------------------------------------

/*
 * UPDATE SAND PHYSICS
 *
 * 1. Accelerate each grain using gravity.
 * 2. Apply granular damping.
 * 3. Randomize the particle update order.
 * 4. Find available physical neighbour positions.
 * 5. Transfer grains into empty neighbours.
 * 6. Resolve collisions and friction.
 * 7. Verify that exactly 64 grains remain.
 */
bool updatePhysics(float dt)
{
    updateParticleVelocities(dt);

    uint8_t particleOrder[SAND_TOTAL] = {};

    buildParticleOrder(particleOrder);

    bool sandMoved = false;

    for (uint8_t i = 0; i < SAND_TOTAL; ++i)
    {
        const uint8_t particleIndex =
            particleOrder[i];

        if (moveParticle(particles[particleIndex], dt)) {
            sandMoved = true;
        }
    }

    // Check that no grains were lost or duplicated.
    if (!validateSandState())
    {
        Serial.println(
            "[SAND GRAVITY] Recovering particle occupancy"
        );

        if (!rebuildOccupancy())
        {
            Serial.println(
                "[SAND GRAVITY] CRITICAL: Recovery failed"
            );
        }

        return true;
    }

    return sandMoved;
}

// ------------------------------------------------------------
// RESET PHYSICS
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
// BEGIN
// ------------------------------------------------------------

void begin()
{
    // Reuse the physical geometry shared with WaterAnimation.
    if (!WaterGeometry::begin())
    {
        initialized = false;

        Serial.println(
            "[SAND GRAVITY] Physical geometry initialization failed"
        );

        return;
    }

    initialized = true;

    reset(millis());

    Serial.println(
        "[SAND GRAVITY] Particle simulation initialized"
    );
}

// ------------------------------------------------------------
// RESET
// ------------------------------------------------------------

void reset(uint32_t nowMs)
{
    if (!WaterGeometry::isReady())
    {
        if (!WaterGeometry::begin())
        {
            initialized = false;
            return;
        }
    }

    initialized = true;

    // Clear all previous particle and gravity state.
    memset(sand, 0, sizeof(sand));
    memset(particles, 0, sizeof(particles));

    resetPhysicsState(nowMs);

    // Obtain the latest MPU6050 gravity direction.
    updateGravityEstimate();

    // Select the chamber opposite to gravity.
    selectSourceModule();

    // Fill that chamber with 64 grains.
    if (!initializeSand())
    {
        initialized = false;

        Serial.println(
            "[SAND GRAVITY] Initial particle placement failed"
        );

        return;
    }

    lastPhysicsMs = nowMs;

    render();

    Serial.printf(
        "[SAND GRAVITY] Initialized: %u grains, source module: %s\n",
        getSandCount(),
        (sourceModule == 0) ? "SLAVE" : "MASTER"
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

    // Update the gravity estimate whenever new MPU6050 data
    // becomes available.
    //
    // Do not reset the particle velocities when gravity changes.
    updateGravityEstimate();

    // Wait until the next physics step.
    const uint32_t elapsedMs =
        (uint32_t)(nowMs - lastPhysicsMs);

    if (elapsedMs < PHYSICS_STEP_MS) {
        return;
    }

    lastPhysicsMs = nowMs;

    float dt = elapsedMs * 0.001f;

    dt = clampFloat(dt, 0.001f, MAX_PHYSICS_DT);

    // Update the complete granular simulation.
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

uint8_t getSandCount()
{
    return countOccupancy();
}

} // namespace SandGravity

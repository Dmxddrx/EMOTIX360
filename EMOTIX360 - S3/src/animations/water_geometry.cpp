
#include "water_geometry.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace WaterGeometry {

// ------------------------------------------------------------
// COMPLETE HOURGLASS PRIORITY MAP
// ------------------------------------------------------------

const uint8_t HOURGLASS_PRIORITY[MATRIX_HEIGHT][MATRIX_WIDTH] = {

    // Upper slave: SOURCE_PRIORITY.
    // Lower master: rotated DESTINATION_PRIORITY.

    {
         1,  2,  4,  6,  8, 10, 14, 19,
        24, 22, 16, 11,  7,  4,  2,  1
    },

    {
         2,  3,  5,  7,  9, 13, 17, 22,
        26, 23, 17, 12,  8,  5,  3,  2
    },

    {
         4,  5,  6,  8, 11, 15, 20, 25,
        28, 25, 18, 13,  9,  6,  5,  4
    },

    {
         6,  7,  8,  9, 12, 18, 24, 27,
        30, 27, 19, 14, 10,  9,  8,  7
    },

    {
         8,  9, 11, 12, 16, 21, 26, 29,
        32, 28, 20, 15, 14, 13, 12, 11
    },

    {
        10, 13, 15, 18, 21, 23, 28, 31,
        33, 29, 21, 20, 19, 18, 17, 16
    },

    {
        14, 17, 20, 24, 26, 28, 30, 32,
        34, 31, 29, 28, 27, 25, 23, 22
    },

    {
        19, 22, 25, 27, 29, 31, 32, 33,
        35, 34, 33, 32, 30, 28, 26, 24
    }
};

// ------------------------------------------------------------
// INTERNAL CONFIGURATION
// ------------------------------------------------------------

namespace {

// Use a unique name to avoid the Arduino PI macro.
constexpr float GEOMETRY_PI =
    3.14159265358979323846f;

constexpr float DEGREES_TO_RADIANS =
    GEOMETRY_PI / 180.0f;

constexpr float MIN_DISTANCE_SQUARED = 0.0001f;

const float MAX_NEIGHBOUR_DISTANCE_SQUARED =
    MAX_NEIGHBOUR_DISTANCE_PITCHES *
    MAX_NEIGHBOUR_DISTANCE_PITCHES;

// All physical LED nodes.
Node nodes[NODE_COUNT] = {};

// Geometry state.
bool geometryReady = false;

uint16_t edgeCount = 0;
uint16_t crossModuleLinkCount = 0;

// ------------------------------------------------------------
// NODE COORDINATE HELPERS
// ------------------------------------------------------------

/*
 * CHECK MODULE
 *
 * Logical MAX7219 mapping:
 *
 *     X = 0..7    Slave.
 *     X = 8..15   Master.
 */
bool isSlaveNode(uint8_t nodeId)
{
    const uint8_t x =
        (uint8_t)(nodeId % MATRIX_WIDTH);

    return x < 8;
}

/*
 * ROTATE AND TRANSLATE MODULE COORDINATES
 *
 * Each module has its own local coordinate system.
 *
 * The local LED coordinates are first centred around:
 *
 *     (3.5, 3.5)
 *
 * The coordinates are then rotated according to the
 * physical mounting direction and translated into the
 * common hourglass coordinate system.
 *
 * Screen coordinate convention:
 *
 *     Positive X = Right.
 *     Positive Y = Down.
 *
 * Positive mathematical angle is clockwise in this
 * screen coordinate convention.
 */
void calculatePhysicalPosition(
    uint8_t logicalX,
    uint8_t logicalY,
    float &physicalX,
    float &physicalY
)
{
    const bool slave =
        logicalX < 8;

    // Convert the complete framebuffer coordinate into
    // the local coordinates of the selected module.
    const uint8_t localX =
        (uint8_t)(logicalX % 8);

    const uint8_t localY =
        logicalY;

    // Centre the local 8x8 LED grid.
    const float localOffsetX =
        (float)localX - 3.5f;

    const float localOffsetY =
        (float)localY - 3.5f;

    // Select the actual physical module rotation.
    const float rotationDeg =
        slave
            ? SLAVE_ROTATION_DEG
            : MASTER_ROTATION_DEG;

    const float angle =
        rotationDeg * DEGREES_TO_RADIANS;

    const float cosine =
        cosf(angle);

    const float sine =
        sinf(angle);

    // Rotate the local coordinate.
    const float rotatedX =
        (localOffsetX * cosine) -
        (localOffsetY * sine);

    const float rotatedY =
        (localOffsetX * sine) +
        (localOffsetY * cosine);

    // Place the module centres above and below the neck.
    const float moduleCentreY =
        slave
            ? -(MODULE_CENTER_DISTANCE_PITCHES * 0.5f)
            : +(MODULE_CENTER_DISTANCE_PITCHES * 0.5f);

    // Save the final physical coordinates.
    physicalX = rotatedX;
    physicalY = rotatedY + moduleCentreY;
}

// ------------------------------------------------------------
// NEIGHBOUR GRAPH
// ------------------------------------------------------------

/*
 * ADD NEIGHBOUR CONNECTION
 *
 * Add an undirected connection between two physical LEDs.
 *
 * Each connection is stored in both nodes.
 */
bool addNeighbour(
    uint8_t firstNode,
    uint8_t secondNode,
    float distance
)
{
    Node &first = nodes[firstNode];
    Node &second = nodes[secondNode];

    // Protect each neighbour array from overflow.
    if (first.neighbourCount >= MAX_NEIGHBOURS ||
        second.neighbourCount >= MAX_NEIGHBOURS)
    {
        Serial.println(
            "[WATER GEOMETRY] ERROR: Neighbour capacity exceeded"
        );

        return false;
    }

    // Add the first direction.
    first.neighbours[first.neighbourCount] = secondNode;

    first.neighbourDistances[first.neighbourCount] =
        distance;

    ++first.neighbourCount;

    // Add the reverse direction.
    second.neighbours[second.neighbourCount] = firstNode;

    second.neighbourDistances[second.neighbourCount] =
        distance;

    ++second.neighbourCount;

    ++edgeCount;

    if (isSlaveNode(firstNode) != isSlaveNode(secondNode)) {
        ++crossModuleLinkCount;
    }

    return true;
}

/*
 * BUILD PHYSICAL NEIGHBOURS
 *
 * Compare the physical position of every LED with every
 * other LED.
 *
 * Two LEDs become neighbours when their physical distance
 * is less than or equal to the configured threshold.
 *
 * This automatically creates:
 *
 *     - Eight-connected neighbours within each module.
 *     - Physical connections between the two modules.
 *     - No false connections based on logical adjacency.
 *
 * The graph is built only once.
 */
bool buildNeighbourGraph()
{
    edgeCount = 0;
    crossModuleLinkCount = 0;

    // Start with empty neighbour lists.
    for (uint16_t i = 0; i < NODE_COUNT; ++i) {
        nodes[i].neighbourCount = 0;
    }

    // Compare every unique pair of physical LEDs.
    for (uint16_t first = 0;
         first < NODE_COUNT;
         ++first)
    {
        for (uint16_t second = first + 1;
             second < NODE_COUNT;
             ++second)
        {
            const float dx =
                nodes[second].x - nodes[first].x;

            const float dy =
                nodes[second].y - nodes[first].y;

            const float distanceSquared =
                (dx * dx) + (dy * dy);

            // Ignore overlapping coordinates.
            if (distanceSquared <= MIN_DISTANCE_SQUARED) {
                continue;
            }

            // Ignore physically distant LEDs.
            if (distanceSquared >
                MAX_NEIGHBOUR_DISTANCE_SQUARED)
            {
                continue;
            }

            const float distance =
                sqrtf(distanceSquared);

            if (!addNeighbour(
                    (uint8_t)first,
                    (uint8_t)second,
                    distance))
            {
                return false;
            }
        }
    }

    return true;
}

// ------------------------------------------------------------
// GEOMETRY VALIDATION
// ------------------------------------------------------------

/*
 * VALIDATE PRIORITY REFERENCE
 *
 * This validates that all 128 logical framebuffer positions
 * are represented in the original SandClock mapping.
 *
 * Priority values are NOT used as particle movement weights.
 */
bool validatePriorityReference()
{
    for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y)
    {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x)
        {
            if (HOURGLASS_PRIORITY[y][x] == 0) {

                Serial.printf(
                    "[WATER GEOMETRY] ERROR: Invalid map at X:%u Y:%u\n",
                    x,
                    y
                );

                return false;
            }
        }
    }

    return true;
}

/*
 * VALIDATE GRAPH CONNECTIVITY
 *
 * All 128 LED nodes must belong to one connected graph.
 *
 * This is especially important at the central neck.
 *
 * If the two modules have no cross-module connection,
 * the water cannot travel between the chambers.
 */
bool validateGraphConnectivity()
{
    bool visited[NODE_COUNT] = {};

    uint8_t queue[NODE_COUNT] = {};

    uint16_t queueHead = 0;
    uint16_t queueTail = 0;

    // Start traversal from node zero.
    visited[0] = true;
    queue[queueTail++] = 0;

    while (queueHead < queueTail)
    {
        const uint8_t current =
            queue[queueHead++];

        const Node &node =
            nodes[current];

        for (uint8_t i = 0;
             i < node.neighbourCount;
             ++i)
        {
            const uint8_t neighbour =
                node.neighbours[i];

            if (visited[neighbour]) {
                continue;
            }

            visited[neighbour] = true;

            queue[queueTail++] = neighbour;
        }
    }

    // Every LED must be reachable from the starting node.
    if (queueTail != NODE_COUNT)
    {
        Serial.printf(
            "[WATER GEOMETRY] ERROR: Connected nodes %u/%u\n",
            (unsigned int)queueTail,
            (unsigned int)NODE_COUNT
        );

        return false;
    }

    return true;
}

/*
 * VALIDATE NODE NEIGHBOURS
 *
 * Every LED needs at least one physical connection.
 */
bool validateNodeNeighbours()
{
    for (uint16_t i = 0; i < NODE_COUNT; ++i)
    {
        if (nodes[i].neighbourCount == 0)
        {
            Serial.printf(
                "[WATER GEOMETRY] ERROR: Isolated node %u\n",
                (unsigned int)i
            );

            return false;
        }
    }

    // At least one connection must join the two modules.
    if (crossModuleLinkCount == 0)
    {
        Serial.println(
            "[WATER GEOMETRY] ERROR: No connection across the neck"
        );

        return false;
    }

    return true;
}

} // namespace

// ------------------------------------------------------------
// PUBLIC API
// ------------------------------------------------------------

bool begin()
{
    if (geometryReady) {
        return true;
    }

    Serial.println();
    Serial.println("[WATER GEOMETRY] Building physical LED geometry");

    // Clear all previous node data.
    memset(nodes, 0, sizeof(nodes));

    edgeCount = 0;
    crossModuleLinkCount = 0;

    // Validate the existing SandClock mapping.
    if (!validatePriorityReference()) {
        return false;
    }

    // --------------------------------------------------------
    // CALCULATE PHYSICAL POSITION OF ALL 128 LEDS
    // --------------------------------------------------------

    for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y)
    {
        for (uint8_t x = 0; x < MATRIX_WIDTH; ++x)
        {
            const uint8_t id =
                (uint8_t)(y * MATRIX_WIDTH + x);

            calculatePhysicalPosition(
                x,
                y,
                nodes[id].x,
                nodes[id].y
            );
        }
    }

    // --------------------------------------------------------
    // GENERATE PHYSICAL NEIGHBOURS
    // --------------------------------------------------------

    if (!buildNeighbourGraph()) {
        return false;
    }

    // --------------------------------------------------------
    // VALIDATE COMPLETE PHYSICAL GRAPH
    // --------------------------------------------------------

    if (!validateNodeNeighbours()) {
        return false;
    }

    if (!validateGraphConnectivity()) {
        return false;
    }

    geometryReady = true;

    Serial.printf(
        "[WATER GEOMETRY] Nodes: %u\n",
        (unsigned int)NODE_COUNT
    );

    Serial.printf(
        "[WATER GEOMETRY] Edges: %u\n",
        (unsigned int)edgeCount
    );

    Serial.printf(
        "[WATER GEOMETRY] Cross-module links: %u\n",
        (unsigned int)crossModuleLinkCount
    );

    Serial.printf(
        "[WATER GEOMETRY] Centre separation: %.3f pitches\n",
        MODULE_CENTER_DISTANCE_PITCHES
    );

    Serial.println("[WATER GEOMETRY] Geometry ready");

    return true;
}

bool isReady()
{
    return geometryReady;
}

uint8_t getNodeId(uint8_t x, uint8_t y)
{
    if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) {
        return 0xFF;
    }

    // Node IDs always match the existing MAX7219
    // logical framebuffer coordinates.
    return (uint8_t)(y * MATRIX_WIDTH + x);
}

bool getCoordinates(
    uint8_t nodeId,
    uint8_t &x,
    uint8_t &y
)
{
    if (nodeId >= NODE_COUNT) {
        return false;
    }

    x = (uint8_t)(nodeId % MATRIX_WIDTH);
    y = (uint8_t)(nodeId / MATRIX_WIDTH);

    return true;
}

const Node* getNode(uint8_t nodeId)
{
    if (!geometryReady || nodeId >= NODE_COUNT) {
        return nullptr;
    }

    return &nodes[nodeId];
}

bool areNeighbours(uint8_t firstNode, uint8_t secondNode)
{
    if (!geometryReady ||
        firstNode >= NODE_COUNT ||
        secondNode >= NODE_COUNT)
    {
        return false;
    }

    const Node &node =
        nodes[firstNode];

    for (uint8_t i = 0;
         i < node.neighbourCount;
         ++i)
    {
        if (node.neighbours[i] == secondNode) {
            return true;
        }
    }

    return false;
}

uint8_t getReferencePriority(uint8_t x, uint8_t y)
{
    if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) {
        return 0;
    }

    return HOURGLASS_PRIORITY[y][x];
}

uint16_t getEdgeCount()
{
    return edgeCount;
}

uint16_t getCrossModuleLinkCount()
{
    return crossModuleLinkCount;
}

} // namespace WaterGeometry

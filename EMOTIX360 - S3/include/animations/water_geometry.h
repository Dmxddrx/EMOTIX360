
#pragma once

#include <Arduino.h>

namespace WaterGeometry {

// ------------------------------------------------------------
// LOGICAL MATRIX CONFIGURATION
// ------------------------------------------------------------

// Same logical framebuffer used by the working SandClock.
//
// X = 0..7    Upper slave module.
// X = 8..15   Lower master module.
//
// Y = 0..7    Local row on each module.

constexpr uint8_t MATRIX_WIDTH = 16;
constexpr uint8_t MATRIX_HEIGHT = 8;

constexpr uint16_t NODE_COUNT =
    MATRIX_WIDTH * MATRIX_HEIGHT;

// Maximum number of physically neighbouring LEDs per node.
// Eight are normally available inside a module.
// Additional connections may exist around the central neck.
constexpr uint8_t MAX_NEIGHBOURS = 12;

// ------------------------------------------------------------
// PHYSICAL MODULE CONFIGURATION
// ------------------------------------------------------------

// Coordinates use:
//     Positive X = right.
//     Positive Y = down.
//
// With this convention, positive rotation is clockwise.

// Upper slave is physically rotated 45 degrees anticlockwise.
constexpr float SLAVE_ROTATION_DEG = -45.0f;

// Lower master is physically rotated 45 degrees clockwise.
constexpr float MASTER_ROTATION_DEG = 45.0f;

/*
 * Initial module centre-to-centre separation.
 *
 * Assumption:
 *     The two rotated 8x8 modules are mounted with their
 *     opposing PCB corners meeting.
 *
 * The nominal separation is 8 * sqrt(2) LED pitches.
 *
 * IMPORTANT:
 *     This is a starting mechanical estimate.
 *     Adjust it if the actual LED-centre separation differs.
 */
constexpr float MODULE_CENTER_DISTANCE_PITCHES = 11.3137085f;

/*
 * Maximum distance for two LED centres to be considered
 * physical neighbours.
 *
 * 1.0 pitch:
 *     Horizontal/vertical neighbours within a module.
 *
 * 1.414 pitch:
 *     Diagonal neighbours within a module.
 *
 * The same distance threshold determines cross-module
 * connections at the hourglass neck.
 */
constexpr float MAX_NEIGHBOUR_DISTANCE_PITCHES = 1.52f;

// ------------------------------------------------------------
// PHYSICAL NODE
// ------------------------------------------------------------

struct Node {

    // LED centre in the common physical coordinate system.
    float x;
    float y;

    // Number of connected physical neighbours.
    uint8_t neighbourCount;

    // Node IDs of the connected neighbours.
    uint8_t neighbours[MAX_NEIGHBOURS];

    // Physical distance to each corresponding neighbour.
    float neighbourDistances[MAX_NEIGHBOURS];
};

// ------------------------------------------------------------
// HOURGLASS MAPPING REFERENCE
// ------------------------------------------------------------

/*
 * COMPLETE HOURGLASS PRIORITY MAP
 *
 * This preserves the existing SandClock mapping:
 *
 *     X = 0..7   Upper slave.
 *     X = 8..15  Lower master.
 *
 * Upper slave:
 *     Original SOURCE_PRIORITY.
 *     Existing slave mapping retained.
 *
 * Lower master:
 *     Original DESTINATION_PRIORITY.
 *     Existing 90-degree anticlockwise LED mapping applied.
 *
 * IMPORTANT:
 *
 *     These values are retained for mapping reference
 *     and validation only.
 *
 *     Water particles do NOT use these numbers as movement
 *     priorities.
 *
 *     Physical movement is controlled by the neighbour graph.
 */
extern const uint8_t
HOURGLASS_PRIORITY[MATRIX_HEIGHT][MATRIX_WIDTH];

// ------------------------------------------------------------
// PUBLIC API
// ------------------------------------------------------------

// Calculate all physical LED positions and build the neighbour graph.
bool begin();

// Check whether the physical geometry is ready.
bool isReady();

// Convert logical MAX7219 coordinates into a node ID.
// Returns 0xFF for invalid coordinates.
uint8_t getNodeId(uint8_t x, uint8_t y);

// Convert a node ID into logical MAX7219 coordinates.
bool getCoordinates(
    uint8_t nodeId,
    uint8_t &x,
    uint8_t &y
);

// Get a read-only physical node and its neighbours.
// Returns nullptr for an invalid node ID.
const Node* getNode(uint8_t nodeId);

// Check whether two nodes are physically connected.
bool areNeighbours(uint8_t firstNode, uint8_t secondNode);

// Get the original SandClock reference value at a logical position.
// Returns zero for invalid coordinates.
uint8_t getReferencePriority(uint8_t x, uint8_t y);

// Geometry diagnostics.
uint16_t getEdgeCount();
uint16_t getCrossModuleLinkCount();

} // namespace WaterGeometry

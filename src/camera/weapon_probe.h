#pragma once

namespace NMSHT {

// Locates the nodes the engine hangs off the camera, so the first-person
// weapon can be held to the body instead of swinging with the head.
//
// Nothing served through the camera accessor can fix the viewmodel: every
// accessor consumer is already handed a clean copy and the multitool still
// turns with the view, so its placement reads the transform memory directly the
// way the renderer does. Its own matrix has to be found before it can be fed
// the clean basis.
//
// The scan pins the injected rotation, then searches memory for the exact
// basis it just wrote. Freezing is what makes the search possible at all - the
// camera changes every frame, so an unfrozen pattern has moved on long before
// the scan reaches the page holding a copy of it. Copies sitting at the
// camera's own position are the render path duplicating the transform wholesale
// and say nothing; copies sitting a few centimetres off it are the nodes
// parented to the eye, which is what the multitool and hands are.
void StartWeaponProbe();

} // namespace NMSHT

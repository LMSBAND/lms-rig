// LMS Rig HEAD — the control surface's amp-head shell (contract: ../SPEC.md).
// Goes together like a real head: a sleeve (rounded long edges, the tolex wrap), a recessed faceplate
// with all the controls, a vented back panel carrying the ESP32-S3, a top handle, feet, corner caps, knobs.
// Kobra S1, 250 mm bed. Every part prints flat, no supports:
//   openscad -D 'part="sleeve"' -o head_sleeve.stl head.scad
//   parts: sleeve faceplate badge back handle feet corners knobs inserts   part="assembly" = everything put together
//   inserts = button-hole reducers; set insert_bores to the buttons you have (bushing diameter + 0.3)
//
// Measure before printing (marked MEASURE): the LCD bezel stack, the pot/encoder bushings, the button hole.

part = "assembly";
$fn = 64;

// ---- shell -------------------------------------------------------------------------------------------
W = 240;  H = 165;  D = 140;   // outside: width, height, depth (the top strip of the faceplate carries the logo)
wall = 3;
r_edge = 8;                    // rounding on the four long edges (the ones running front to back)
recess = 5;                    // faceplate and back panel sit this far inside the sleeve
panel_t = 3;
clr = 0.3;                     // panel-to-sleeve clearance per side
col = 10;                      // corner screw blocks inside the sleeve
col_len = 18;
pilot = 2.6;                   // M3 self-tapping into the blocks
m3 = 3.4;                      // M3 clearance
m3_csk = 6.4;                  // countersink on the panel faces

PW = W - 2 * wall - 2 * clr;   // panel size
PH = H - 2 * wall - 2 * clr;
hole_e = col / 2 - clr;        // panel screw centre from the panel edge

// ---- faceplate layout (front view, mm from the panel's bottom-left corner) ------------------------------
lcd_c = [62, 104];              // 20x4 HD44780: window centre
lcd_pcb = [98, 60];  lcd_holes = [93, 55];
lcd_view = [78, 27];           // window cut to the viewing area; the metal bezel hides behind the panel
lcd_stack = 9.5;               // MEASURE: PCB front face to bezel front face (the standoff height)
sel_x = [137, 174, 211];  sel_y = 112;   // AMP, CAB A, CAB B push encoders
soft_y = 68;                              // soft encoders, under the selectors
btn_x = [26, 50, 74, 98];  btn_y = 60;    // 4 soft buttons (12 mm LED-ring momentaries); 4th is PAGE
pot_pitch = 36;  pot_y = 36;              // GAIN BASS MIDDLE TREBLE PRESENCE MASTER
pot_x = [for (i = [0 : 5]) PW / 2 + (i - 2.5) * pot_pitch];
bush_hole = 7.4;               // MEASURE: M7 bushing (EC11 encoders, RV16 pots)
btn_hole = 12.2;               // MEASURE: button body
engrave = 0.6;
logo_c = [PW / 2, PH - 15];    // LMS RIG badge centre
logo_size = 17;
badge_pin = 1.9;              // 1.75 filament offcuts as locating pins
badge_pins = [-50, 50];
font = "Liberation Sans:style=Bold";

// ---- back panel ----------------------------------------------------------------------------------------
dev = [69, 26];                // ESP32-S3-DevKitC-1 board, USB-C end against the panel
usb_x = 60;                    // board centre, from the back panel's left edge seen from behind
shelf_y = 18;                  // shelf top, from the panel's bottom edge
usb_above_shelf = 14;          // board sits on its headers: 8.5 pins + 2.5 plastic + PCB + half the port
usb_slot = [30, 10];           // covers both USB-C ports

// ---- handle, feet, corners, knobs ----------------------------------------------------------------------
handle_span = 112;             // screw spacing
handle_w = 20;                 // across
foot_d = 20;  foot_h = 7;  foot_in = 22;   // foot centres this far in from the sides and front/back
cap = 26;  cap_t = 2.2;        // corner caps
shaft_d = 6.15;  shaft_flat = 4.6;         // D-shaft bore
insert_bores = [7.1, 7.1];                 // button-hole reducers: one row of 4 per bore (buttons measured 6.7, 7.1 fits)
insert_od = btn_hole + 0.1;                // interference in the 12.2 hole; the slotted stem squeezes in
insert_flange = 16;  insert_flange_t = 0.8;
insert_barb = 0.3;                         // snap lip past the stem, each side (0 = none; the buttons have 6.3 mm of thread)
insert_barb_h = 0.6;                       // whole lip, catch face + lead-in, past the back of the panel
insert_slots = 4;                          // slots so the stem fingers flex on the way in

//==========================================================================================================
module rbox(size, r) {         // box with the four y-running edges rounded
  hull() for (x = [r, size.x - r], z = [r, size.z - r]) translate([x, 0, z]) rotate([-90, 0, 0]) cylinder(r = r, h = size.y);
}

module m3_csk_hole() {         // through hole, countersink on the +z face
  translate([0, 0, -1]) cylinder(d = m3, h = panel_t + 2);
  translate([0, 0, panel_t - (m3_csk - m3) / 2]) cylinder(d1 = m3, d2 = m3_csk, h = (m3_csk - m3) / 2 + 0.01);
}

module panel_corner_holes() {
  for (x = [hole_e, PW - hole_e], y = [hole_e, PH - hole_e]) translate([x, y, 0]) m3_csk_hole();
}

module label(t, at, size = 4.2, halign = "center") {
  translate([at.x, at.y, panel_t - engrave]) linear_extrude(engrave + 1) text(t, size = size, font = font, halign = halign, valign = "center");
}

//---- sleeve, in assembly coordinates: x across, y front (0) to back (D), z up ---------------------------
module sleeve() {
  difference() {
    union() {
      difference() {
        rbox([W, D, H], r_edge);
        translate([wall, -1, wall]) cube([W - 2 * wall, D + 2, H - 2 * wall]);
      }
      for (cx = [0, 1], cz = [0, 1]) corner_blocks(cx, cz);
    }
    // handle screws up through the top, feet bolts through the bottom
    for (s = [-1, 1]) translate([W / 2 + s * handle_span / 2, D / 2, H - wall - 1]) cylinder(d = m3, h = wall + 2);
    for (x = [foot_in, W - foot_in], y = [foot_in, D - foot_in]) translate([x, y, -1]) cylinder(d = m3, h = wall + 2);
  }
}

module corner_blocks(cx, cz) {
  // block origin at the inside corner, growing inward
  x0 = cx ? W - wall - col : wall;
  z0 = cz ? H - wall - col : wall;
  yf = recess + panel_t;            // faceplate seat
  yb = D - recess - panel_t;        // back panel seat
  kx = cx ? W - wall : wall;        // the corner line itself
  kz = cz ? H - wall : wall;
  difference() {
    union() {
      // front block, with a 45 degree gusset behind it so it prints standing up without support
      hull() {
        translate([x0, yf, z0]) cube([col, col_len, col]);
        translate([kx - (cx ? 0.01 : 0), yf, kz - (cz ? 0.01 : 0)]) cube([0.01, col_len + col, 0.01]);
      }
      translate([x0, yb - col_len, z0]) cube([col, col_len, col]);
    }
    for (y = [yf - 1, yb - 14]) translate([x0 + col / 2, y, z0 + col / 2]) rotate([-90, 0, 0]) cylinder(d = pilot, h = 15);
  }
}

//---- faceplate, front-view coordinates: front face at z = panel_t, the back side toward -z -------------
module faceplate() {
  difference() {
    union() {
      cube([PW, PH, panel_t]);
      for (sx = [-1, 1], sy = [-1, 1])   // LCD standoffs
        translate([lcd_c.x + sx * lcd_holes.x / 2, lcd_c.y + sy * lcd_holes.y / 2, -lcd_stack]) cylinder(d = 6.5, h = lcd_stack + 0.01);
    }
    panel_corner_holes();
    // LCD window, chamfered toward the front
    translate([lcd_c.x, lcd_c.y, 0]) hull() {
      translate([0, 0, -0.01]) cube([lcd_view.x, lcd_view.y, 0.02], center = true);
      translate([0, 0, panel_t]) cube([lcd_view.x + 2 * panel_t, lcd_view.y + 2 * panel_t, 0.02], center = true);
    }
    for (sx = [-1, 1], sy = [-1, 1])
      translate([lcd_c.x + sx * lcd_holes.x / 2, lcd_c.y + sy * lcd_holes.y / 2, -lcd_stack - 1]) cylinder(d = pilot, h = lcd_stack - 0.5);
    for (x = sel_x) { translate([x, sel_y, -1]) cylinder(d = bush_hole, h = panel_t + 2); translate([x, soft_y, -1]) cylinder(d = bush_hole, h = panel_t + 2); }
    for (x = pot_x) translate([x, pot_y, -1]) cylinder(d = bush_hole, h = panel_t + 2);
    for (x = btn_x) translate([x, btn_y, -1]) cylinder(d = btn_hole, h = panel_t + 2);

    // engraving: piping line, brand, labels (a filament swap at 0.6 mm colours it all in)
    translate([0, 0, panel_t - engrave]) linear_extrude(engrave + 1) difference() {
      offset(delta = -3.0) square([PW, PH]);
      offset(delta = -4.2) square([PW, PH]);
    }
    for (x = badge_pins) translate([logo_c.x + x, logo_c.y - 1, -1]) cylinder(d = badge_pin, h = panel_t + 2);
    names = ["GAIN", "BASS", "MIDDLE", "TREBLE", "PRESENCE", "MASTER"];
    for (i = [0 : 5]) label(names[i], [pot_x[i], pot_y - 18.5], 4);
    sel_names = ["AMP", "CAB A", "CAB B"];
    for (i = [0 : 2]) label(sel_names[i], [sel_x[i], sel_y - 15], 4);
    for (i = [0 : 2]) label(str(i + 1), [sel_x[i] + 14, soft_y - 9], 3.4);
    label("PAGE", [btn_x[3], btn_y - 11], 3.4);
  }
}

//---- LMS RIG badge: raised letters on an outlined plate with an underline bar, printed face up -----------
module logo_2d() {
  text("LMS RIG", size = logo_size, font = font, halign = "center", valign = "center", spacing = 1.08);
  translate([-62, -logo_size / 2 - 5]) polygon([[0, 0], [124, 0], [128, 3.2], [4, 3.2]]);   // underline swoosh
}
module badge() {
  difference() {
    union() {
      linear_extrude(1.8) offset(r = 2.6) logo_2d();
      linear_extrude(3.4) logo_2d();
    }
    for (x = badge_pins) translate([x, -1, -0.01]) cylinder(d = badge_pin, h = 1.4);
  }
}

//---- back panel, seen from behind: outside face at z = panel_t, inside toward -z ------------------------
module back() {
  difference() {
    union() {
      cube([PW, PH, panel_t]);
      // shelf the DevKit sits on (on its headers), zip-tied down, side guides to locate it
      translate([usb_x - dev.y / 2 - 4, shelf_y - 2.5, -dev.x - 4]) cube([dev.y + 8, 2.5, dev.x + 4 + 0.01]);
      for (s = [-1, 1]) translate([usb_x + s * (dev.y / 2 + 1.25) - 1, shelf_y - 0.01, -dev.x - 4]) cube([2, 5, dev.x + 4 + 0.01]);
      for (s = [-1, 1])   // gussets under the shelf
        translate([usb_x + s * (dev.y / 2 + 2), 0, 0]) rotate([0, -90, 0]) translate([0, 0, -1])
          linear_extrude(2) polygon([[0.01, shelf_y - 2.5], [0.01, 2], [-dev.x * 0.6, shelf_y - 2.5]]);
    }
    panel_corner_holes();
    translate([usb_x - usb_slot.x / 2, shelf_y + usb_above_shelf - usb_slot.y / 2, -1]) cube([usb_slot.x, usb_slot.y, panel_t + 2]);
    for (z = [-dev.x * 0.25, -dev.x * 0.75], s = [-1, 1])   // zip-tie slots
      translate([usb_x + s * (dev.y / 2 + 2.25) - 1.5, shelf_y - 4, z - 2.5]) cube([3, 6, 5]);
    // vents: slots across the upper half
    for (r = [0 : 4], c = [0 : 9]) translate([PW - 18 - c * 15, PH - 24 - r * 13, -1]) hull() {
      cylinder(d = 4, h = panel_t + 2);
      translate([-9, 0, 0]) cylinder(d = 4, h = panel_t + 2);
    }
    label("LMS RIG  HEAD", [usb_x, shelf_y + usb_above_shelf + 12], 5);
    label("USB", [usb_x, shelf_y + usb_above_shelf - 10], 3.4);
  }
}

//---- handle: printed on its side; M3 from inside the sleeve up into each foot -------------------------
module handle() {
  L = handle_span + 22;
  difference() {
    linear_extrude(handle_w) difference() {
      hull() {
        square([L, 1]);
        for (x = [10, L - 10]) translate([x, 24]) circle(r = 10);
      }
      translate([(L - 90) / 2, -1]) offset(r = 6) offset(delta = -6) square([90, 23]);
    }
    for (x = [11, L - 11]) translate([x, -1, handle_w / 2]) rotate([-90, 0, 0]) cylinder(d = pilot, h = 14);
  }
}

module feet() {
  for (i = [0 : 3]) translate([i * (foot_d + 4), 0, 0]) difference() {
    cylinder(d1 = foot_d, d2 = foot_d - 3, h = foot_h);
    translate([0, 0, -1]) cylinder(d = m3, h = foot_h + 2);
    translate([0, 0, -0.01]) cylinder(d = 6, h = 3.2, $fn = 6);   // M3 nut, from underneath
  }
}

//---- corner caps: amp hardware for the front and back corners, glued on --------------------------------
module corner_cap() {
  t = cap_t;
  difference() {
    intersection() {
      translate([-t, -t, -t]) cube([cap, cap, cap]);
      translate([-t, -t, -t]) rbox([W + 2 * t, D + 2 * t, H + 2 * t], r_edge + t);
    }
    rbox([W, D, H], r_edge);
    translate([wall + 2.5, -t - 1, wall + 2.5]) cube([cap, cap, cap]);   // leave the opening and the panel screws clear
  }
}
module corners() {   // 8, printed on their flat front face so the rounded edge stands vertical
  for (i = [0 : 7]) translate([(i % 4) * (cap + 6), floor(i / 4) * (cap + 6), 0]) translate([cap_t, cap - cap_t, cap_t]) rotate([90, 0, 0]) corner_cap();
}

//---- button-hole reducers: top hat, flange on the panel face; slotted stem with a barb snaps in behind the
//     panel, and the button's own nut then clamps it for good ------------------------------------------------
module insert(bore) {
  zt = insert_flange_t + panel_t;          // back face of the panel
  difference() {
    union() {
      cylinder(d = insert_flange, h = insert_flange_t);
      cylinder(d = insert_od, h = zt + 0.1);
      if (insert_barb > 0) {
        translate([0, 0, zt + 0.1]) cylinder(d = insert_od + 2 * insert_barb, h = 0.15);                             // catch face
        translate([0, 0, zt + 0.25]) cylinder(d1 = insert_od + 2 * insert_barb, d2 = insert_od - 0.4, h = insert_barb_h - 0.25);  // lead-in
      }
    }
    translate([0, 0, -1]) cylinder(d = bore, h = zt + 4);
    translate([0, 0, -0.01]) cylinder(d1 = bore + 0.8, d2 = bore, h = 0.4);   // front chamfer
    for (a = [0 : insert_slots - 1]) rotate(a * 360 / insert_slots)
      translate([0, -0.6, insert_flange_t + 0.6]) cube([insert_od, 1.2, zt + 4]);
  }
}
module inserts() {   // printed flange down; bore size engraved in the flange so the rows don't get mixed up
  for (r = [0 : len(insert_bores) - 1], i = [0 : 3]) translate([i * (insert_flange + 4), r * (insert_flange + 6), 0]) insert(insert_bores[r]);
  for (r = [0 : len(insert_bores) - 1]) translate([-11, r * (insert_flange + 6), 0])
    linear_extrude(0.6) text(str(insert_bores[r]), size = 4, font = font, halign = "right", valign = "center");
}

//---- knobs ---------------------------------------------------------------------------------------------
module d_bore(h) {
  intersection() {
    cylinder(d = shaft_d, h = h);
    translate([-shaft_d / 2, -shaft_d / 2, 0]) cube([shaft_d, shaft_flat + 0.0 - shaft_d / 2 + shaft_d / 2, h]);
  }
}
module amp_knob() {            // the pots: skirt, knurled body, pointer line
  difference() {
    union() {
      cylinder(d = 28, h = 3);
      cylinder(d1 = 23, d2 = 21, h = 16);
    }
    for (a = [0 : 15 : 359]) rotate(a) translate([11.6, 0, 3.5]) cylinder(d = 1.6, h = 20, $fn = 8);
    translate([0, 0, -0.01]) d_bore(12);
    translate([0, 3, 16 - 0.8]) cube([1.6, 14, 2], center = true);           // top pointer
    rotate(90) translate([12.6, 0, 3 - 0.8]) cube([3, 1.6, 2], center = true); // skirt pointer
  }
}
module chicken_head() {        // the selectors
  difference() {
    union() {
      cylinder(d = 18, h = 14);
      hull() { cylinder(d = 14, h = 9); translate([0, 17, 0]) cylinder(d = 5, h = 9); }
    }
    translate([0, 0, -0.01]) d_bore(12);
    translate([0, 12, 9 - 0.8]) cube([1.6, 9, 2], center = true);
  }
}
module soft_knob() {
  difference() {
    cylinder(d = 19, h = 15);
    for (a = [0 : 20 : 359]) rotate(a) translate([9.8, 0, 2]) cylinder(d = 1.8, h = 20, $fn = 8);
    translate([0, 0, -0.01]) d_bore(12);
    translate([0, 3, 15 - 0.8]) cube([1.6, 9, 2], center = true);
  }
}
module knobs() {
  for (i = [0 : 5]) translate([(i % 3) * 34, floor(i / 3) * 34, 0]) amp_knob();
  for (i = [0 : 2]) translate([110 + i * 28, 0, 0]) chicken_head();
  for (i = [0 : 2]) translate([110 + i * 26, 40, 0]) soft_knob();
}

//---- assembly ------------------------------------------------------------------------------------------
module at_front() { translate([wall + clr, recess + panel_t, wall + clr]) rotate([90, 0, 0]) children(); }
module at_back() { translate([W - wall - clr, D - recess - panel_t, wall + clr]) rotate([90, 0, 180]) children(); }

module assembly() {
  color("#2b2622") sleeve();
  color("#d8c9a8") at_front() faceplate();
  color("#3a3430") at_back() back();
  color("#c9a227") at_front() translate([logo_c.x, logo_c.y, panel_t]) badge();
  color("#2b2622") translate([W / 2 - (handle_span + 22) / 2, D / 2 + handle_w / 2, H]) rotate([90, 0, 0]) handle();
  for (x = [foot_in, W - foot_in], y = [foot_in, D - foot_in]) color("#111") translate([x, y, 0]) mirror([0, 0, 1]) cylinder(d1 = foot_d, d2 = foot_d - 3, h = foot_h);
  for (cx = [0, 1], cy = [0, 1], cz = [0, 1])
    color("#b8b8b8") translate([cx * W, cy * D, cz * H]) mirror([cx, 0, 0]) mirror([0, cy, 0]) mirror([0, 0, cz]) corner_cap();
  // controls (ghosts)
  at_front() {
    color("#5a8f5a") translate([lcd_c.x, lcd_c.y, panel_t / 2]) cube([lcd_view.x, lcd_view.y, panel_t], center = true);
    for (x = pot_x) color("#e8e2d0") translate([x, pot_y, panel_t]) amp_knob();
    for (x = sel_x) color("#e8e2d0") translate([x, sel_y, panel_t]) chicken_head();
    for (x = sel_x) color("#e8e2d0") translate([x, soft_y, panel_t]) soft_knob();
    for (x = btn_x) color("#c03030") translate([x, btn_y, panel_t]) cylinder(d = 12, h = 4);
  }
}

//---- print orientations --------------------------------------------------------------------------------
if (part == "assembly") assembly();
else if (part == "sleeve") translate([0, 0, D]) rotate([-90, 0, 0]) sleeve();       // front opening up
else if (part == "faceplate") translate([PW, 0, panel_t]) rotate([0, 180, 0]) faceplate(); // face down
else if (part == "badge") badge();
else if (part == "back") translate([PW, 0, panel_t]) rotate([0, 180, 0]) back();          // face down
else if (part == "handle") handle();
else if (part == "feet") feet();
else if (part == "corners") corners();
else if (part == "knobs") knobs();
else if (part == "inserts") inserts();

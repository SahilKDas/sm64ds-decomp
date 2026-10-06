sm64ds.pack {
  id = "probe",
  name = "Probe Pack",
  author = "64DS-DX test suite",
  version = "2",
  license = "CC0-1.0",
  provenance = "generated test fixture"
}

sm64ds.character {
  id = 4,
  key = "probe",
  name = "Probe",
  base = 2,
  body = "body.bmd",
  head_cap = "head-cap.bmd",
  head_no_cap = "head-no-cap.bmd",
  hitbox = { radius = 61, height = 110, hurt_radius = 59, hurt_height = 117 },
  animations = { idle = "idle.bca", run = "run.bca" },
  preview = { animation = "idle", icon = "icon.png", yaw = 0, pitch = 0 }
}

sm64ds.texture {
  target = "0123456789abcdef",
  source = "texture.png"
}

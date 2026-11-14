# Presets

Files PiArcBoot adds to a game as it installs it, so the game starts the way it plays best in PiArcBoot's window. A preset is usually a choices file, the same file the game itself saves when you set an option and choose Save.

Each folder is named after a game's id, as the Classics list in `src/classics.c` has it, and holds files laid out as they are in the game's own folder. File names follow HostFS's rules, as the games' zips and ROO311's support pack do. A RISC OS `/` is a `.` here (`Read/Me` is `Read.Me`), and a file's type follows a comma (`Config,ffd` is a data file named `Config`). A file with no type is text. A preset takes the place of the game's own file of the same name, or is added beside the game's files if it has none.

Presets go with each release, beside the program, and apply when a Classics game is installed, whether it goes on floppy images or in a HostFS folder. A game installed before its preset was added gets it when it is installed again.

| Game | Preset | What it does |
| --- | --- | --- |
| `Star-Fighter-3000` | `!Star3000/Choices/Config,ffd` | Starts the game full screen, where the game's default is a desktop window. It is the game's own `Defaults.Config` with byte 32, the Initial display mode in its Choices window, set to 1. With no `Choices$Path`, the game reads `!Star3000.Choices` before `!Star3000.Defaults`. |

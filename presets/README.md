# Presets

What PiArcBoot knows about particular Classics games, kept here rather than in its code. Presets go with each release, beside the program, and apply when a game is installed or started. A game installed before its preset was added gets the preset's files when it is installed again.

Each game can have two things, both named after its id as the Classics tab has it (`Star-Fighter-3000`, or `pkg-` and the package name for a PackMan game).

## `<game>/`, files added to the game

A folder of files that Install adds to the game, such as a choices file, the same file the game itself saves when you set an option and choose Save. It is laid out as the game's folder is once installed, and its names follow HostFS's rules, as the games' zips and ROO311's support pack do. A RISC OS `/` is a `.` here (`Read/Me` is `Read.Me`), and a file's type follows a comma (`Config,ffd` is a data file named `Config`). A file with no type is text. A preset file takes the place of the game's own file of the same name, or is added beside the game's files if it has none.

A game with a support pack has the pack's `!Boot` beside it, which sets `Choices$Path` and `Choices$Write`, so its choices go where RISC OS keeps them, in `!Boot/Choices/<the application's choices folder>`.

## `<game>.ini`, settings for PiArcBoot

`key=value` lines, with `#` starting a comment line that says why.

| Setting | What it does |
| --- | --- |
| `support=<pack>` | Installs the game in a folder that HostFS shows the machine, with ROO311's support pack of that name beside it (`toolbox`), rather than on floppy images |
| `keys=<keys>` | Presses these keys once the game has started, behind the splash screen, for a choice the game doesn't keep from one time to the next. The names are those of the test scripts (`esc`, `enter`, `space`, a letter) |
| `skip=<why>` | Leaves a PackMan game out of the Classics tab, for a package that looks as if it should run on RISC OS 3.11 and doesn't |

## The presets

| Game | Preset | Why |
| --- | --- | --- |
| Star Fighter 3000 | `support=toolbox` | Release 3.21 needs the nested Window Manager and the Toolbox, and is too big for an 800K floppy |
| | `!Boot/Choices/Star3000/Config,ffd` | Starts the game full screen, where its default is a desktop window. It is the game's own `Defaults.Config` with byte 32, the Initial display mode in its Choices window, set to 1 |
| Meteors | `keys=esc` | Escape switches it between its window and the whole screen, and it starts in the window each time |
| Puzzle | `support=toolbox` | It reads its choices from `Choices:` and stops with "Error reading user choices" where there is no such path. The support pack's `!Boot` provides one |
| Hopper, MineHunt | `skip` | They want SharedCLibrary 5.45. ROO311 has 4.79, which the ROM's own C modules call directly, so a newer one can't take its place |
| Mitosis | `skip` | A Lua 5.2 program built for SharedCLibrary 5.17 |
| Freepat | `skip` | It asks for a 4500K WimpSlot, more than a 4MB machine has free |
| MagicMush | `skip` | It changes to 800 by 600 in 256 colours, which an Archimedes' video controller can't show |
| Hangman | `skip` | It stops unless Impression's spelling module is loaded |

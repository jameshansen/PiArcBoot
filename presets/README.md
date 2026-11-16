# Presets

What PiArcBoot knows about particular games, kept here rather than in its code. Presets go with each release, beside the program, and apply when a game is installed or started. A game installed before its preset was added gets the preset when it is installed again.

Each game can have up to three of these, all named after its id: its JASPP package name (`Black-Angel`), or as the Classics tab has it (`Star-Fighter-3000`, or `pkg-` and the package name for a PackMan game).

## `<game>/`, files added to the game

A folder of files that Install adds to the game, such as a choices file, the same file the game itself saves when you set an option and choose Save. It is laid out as the game's folder is once installed, and its names follow HostFS's rules, as the games' zips and ROO311's support pack do. A RISC OS `/` is a `.` here (`Read/Me` is `Read.Me`), and a file's type follows a comma (`Config,ffd` is a data file named `Config`). A file with no type is text. A preset file takes the place of the game's own file of the same name, or is added beside the game's files if it has none.

A game with a support pack has the pack's `!Boot` beside it, which sets `Choices$Path` and `Choices$Write`, so its choices go where RISC OS keeps them, in `!Boot/Choices/<the application's choices folder>`.

## `<game>.install`, what a hard disc installer copies

Some games came with an installer that copies them onto a hard disc. This list says what it copies from the game's discs and where it goes, and Install copies those files from the disc images straight into a folder that HostFS shows the machine, with no installer to run. A copy of the folder is kept in the game's `installed/` folder, and the game starts from its `hostfs/` folder, which a fresh copy of `installed/` replaces whenever it is missing. A game whose files can't be copied still plays from its floppies. Disc images added with Add Game get the same, once the game has every disc the list names, when their first disc matches a game's `match=` setting.

A line each, with `#` starting a comment line:

```
copy <disc> <from> <to>
join <disc> <from> <to>
folder <to>
```

`<disc>` is the game's disc number, from 1. `<from>` is a folder or a file on that disc, as RISC OS writes it from the disc's root, `$`, with `#` for any one character such as a hard space, and without regard to case. A folder copies with everything in it, and `$` alone is the whole disc. `<to>` is where it goes from HostFS's root, `$`, named as the installer named it, and it runs to the end of the line. The lines go in order and a later copy takes the place of an earlier file of the same name, as when an installer copies the same folder from two discs. `join` puts the file `<from>` on the end of the file `<to>` copied before it, for a file an installer joins from parts on two discs, and `folder` makes an empty folder, for one the installer makes for the game's own saves.

`piarcboot ls <image>` lists every file on a disc image with its length and load and execution addresses, and `tests/installers.sh` runs each game's real installer in the emulator, worked by a script in `tests/installers/`, to check its list against what the installer copies, or to see what a new one copies.

## `<game>.ini`, settings for PiArcBoot

`key=value` lines, with `#` starting a comment line that says why.

| Setting | What it does |
| --- | --- |
| `support=<pack>` | Installs the game in a folder that HostFS shows the machine, with ROO311's support pack of that name beside it (`toolbox`), rather than on floppy images |
| `keys=<keys>` | Presses these keys a second after the game has started, behind the splash screen, for a choice the game doesn't keep from one time to the next. For a game that plays in the desktop they come once it has loaded and before its icon is clicked, to answer a question it asks first. The names are those of the scripts (`esc`, `enter`, `space`, a letter) |
| `mode=<number>` | Starts the desktop in this screen mode, for a game that plays in the desktop and asks for a mode, such as `15` for 640 by 256 in 256 colours. Its icon is clicked for it once it has had 2 seconds to set itself up and its floppy drive has been still for 1, and a change of mode for its loading screen isn't taken as the game taking over the screen |
| `crop=1` | Shows the game cropped to its windows (Crop to Desktop Window in the Settings menu), for a game that plays in a desktop window. A choice made in the menu takes its place. With no `mode=`, the desktop is 640 by 512 in 256 colours, mode 21, as the Classics' is, and the game is treated as one that plays in the desktop, as `mode=` does |
| `click=0` | Leaves a game that starts as a task in the desktop waiting there, with its icon on the icon bar unclicked, for a game that wants something else first. It is shown as soon as its icon is up |
| `match=<names>` | Gives disc images added with Add Game this game's copy list and its other settings when the root of their first disc holds all these names, with `#` for any one character. The game's `game.ini` then says `preset=<game>` |
| `skip=<why>` | Leaves a PackMan game out of the Classics tab, for a package that looks as if it should run on RISC OS 3.11 and doesn't |
| `apps=<folder>` | Installs the applications in this folder of a JASPP game's package (`Apps/Games/`) in a folder that HostFS shows the machine, as a Classics game's can, with the game's preset files laid over them, and puts the package's disc images in the drive. For Xenon 2, whose package holds its first disc only as its application, beside the image of its copy protected second disc |

## The presets

| Game | Preset | Why |
| --- | --- | --- |
| Star Fighter 3000 | `support=toolbox` | Release 3.21 needs the nested Window Manager and the Toolbox, and is too big for an 800K floppy |
| | `!Boot/Choices/Star3000/Config,ffd` | Starts the game full screen, where its default is a desktop window. It is the game's own `Defaults.Config` with byte 32, the Initial display mode in its Choices window, set to 1 |
| Meteors | `keys=esc` | Escape switches it between its window and the whole screen, and it starts in the window each time |
| Puzzle | `support=toolbox` | It reads its choices from `Choices:` and stops with "Error reading user choices" where there is no such path. The support pack's `!Boot` provides one |
| Black Angel, Break 147 & Superpool, Carnage Inc, Drop Ship, The Exotic Adventures of Sylvia Layne, Haunted House, Saloon Cars Deluxe, Stunt Racer 2000 | `.install`, `match=` | What their Hard Disc Installer copies: each game's folder from disc 1 and its data from the other discs, with some folders renamed |
| Raw Power | `.install`, `match=` | What its `!RawInst` copies, disc 2 before disc 1 |
| Black Angel | `mode=15` | It plays in the desktop and asks for MODE 15 by name |
| Haunted House, Mah-Jong Patience, Premier Manager, SimCity | `crop=1` | They play in desktop windows and have no full screen option. Haunted House and Premier Manager ask for a 256 colour mode, which the 640 by 512 desktop is. Premier Manager's window is the size of the screen, so cropping leaves out the icon bar. Mah-Jong Patience deals its board and SimCity opens its New City window when its icon is clicked |
| SimCity | `keys=esc` | It asks whether to change to Mode 12, the 16 colour one it was made for, each time it starts. Escape answers No, and it plays the same at 640 by 512 |
| Virtual Golf | `click=0` | It waits for a course from its second disc, which double-clicking `!Wentworth` there gives it. Clicking its icon before then only says it can't see one |
| Break 147 & Superpool, Drop Ship, Saloon Cars Deluxe | `keys=enter` | Installed, they ask for the original disc 1 in drive 0 each time they start, and disc 1 is always there |
| Hopper, MineHunt | `skip` | They want SharedCLibrary 5.45. ROO311 has 4.79, which the ROM's own C modules call directly, so a newer one can't take its place |
| Mitosis | `skip` | A Lua 5.2 program built for SharedCLibrary 5.17 |
| Freepat | `skip` | It asks for a 4500K WimpSlot, more than a 4MB machine has free |
| MagicMush | `skip` | It changes to 800 by 600 in 256 colours, which an Archimedes' video controller can't show |
| Hangman | `skip` | It stops unless Impression's spelling module is loaded |

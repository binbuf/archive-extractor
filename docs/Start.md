Archive Extractor is a MacOS like utility to extract archives but for Windows 11/10. 

Native C++ for deep OS integration.

Double clicking any of these archives will open our extractor:

zip, 7z, zst, rar, xz, gz, lz4, tar, bz2, br

for files like tar.gz where we will conduct both operations tar and gz expand one right after the other.

* The extractor just opens a smals rectangular dialog in the center of the screen. The dialog will just have a label saying `Expanding '<filename>'`. There will be a cancel button next to a progress bar just below the label.

* The extractor works by looking at the archive, determining if it's just one file, then it'll just extract that file into the current working directory. If it's multiple files, then it'll create a folder with the name of the archive and then put the files in that and the current working directory. If it's a archive with the folder already in it, then it'll just take that folder and extract it out into the current working directory. If it's multiple folders at the root, then it will put all those into a folder named after the archive.

* We support password protected archives where feasible

* Register to OS that we handle these extensions

* When extraction completes open the current working directory if it's not already open, or if it is open use that window, bring it to focus/front, and then pre select the newly extracted assets by selecting the new file or folder for the user to easily find and click into
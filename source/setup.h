/* setup.h -- first-run installer and loading splash */

#ifndef __SETUP_H__
#define __SETUP_H__

// 1 if libBully.so, libc++_shared.so and assets/ are in place (and no install was interrupted)
int setup_is_installed(void);

// installer UI; 1 = installed and the user chose to launch, 0 = exit
int setup_run(void);

// draws the loading splash once; it stays up until the game's first frame
void splash_show(void);

#endif

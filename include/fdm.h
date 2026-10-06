#ifndef LM_FDM_H
#define LM_FDM_H

#include <stdint.h>

struct fdm;
enum fdm_add_flag { FDM_AUTOCLOSE = 1 << 10 };

int fdm_init(struct fdm *fdm);
void fdm_destroy(struct fdm *fdm);

int fdm_wait(struct fdm *fdm);

int fdm_add(struct fdm *fdm, uint32_t fd, uint32_t events, void *data,
            int32_t (*cb)(struct fdm *, void *), uint32_t flags);
void fdm_remove(struct fdm *fdm, uint32_t fd);

#endif // !LM_FDM_H

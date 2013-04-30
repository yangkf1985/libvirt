/*
 * xen_driver.c: Unified Xen driver.
 *
 * Copyright (C) 2007-2013 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Richard W.M. Jones <rjones@redhat.com>
 */

#include <config.h>

/* Note:
 *
 * This driver provides a unified interface to the five
 * separate underlying Xen drivers (xen_internal,
 * xend_internal, xs_internal and xm_internal).  Historically
 * the body of libvirt.c handled the five Xen drivers,
 * and contained Xen-specific code.
 */

#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <xen/dom0_ops.h>

#include "virerror.h"
#include "virlog.h"
#include "datatypes.h"
#include "xen_driver.h"

#include "xen_sxpr.h"
#include "xen_xm.h"
#include "xen_hypervisor.h"
#include "xend_internal.h"
#include "xs_internal.h"
#include "xm_internal.h"
#if WITH_XEN_INOTIFY
# include "xen_inotify.h"
#endif
#include "virxml.h"
#include "viralloc.h"
#include "node_device_conf.h"
#include "virpci.h"
#include "viruuid.h"
#include "fdstream.h"
#include "virfile.h"
#include "viruri.h"
#include "vircommand.h"
#include "virnodesuspend.h"
#include "nodeinfo.h"
#include "configmake.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_XEN
#define XEN_SAVE_DIR LOCALSTATEDIR "/lib/libvirt/xen/save"

static int
xenUnifiedNodeGetInfo(virConnectPtr conn, virNodeInfoPtr info);
static int
xenUnifiedDomainGetMaxVcpus(virDomainPtr dom);
static int
xenUnifiedDomainGetVcpus(virDomainPtr dom,
                         virVcpuInfoPtr info, int maxinfo,
                         unsigned char *cpumaps, int maplen);


/* The five Xen drivers below us. */
static struct xenUnifiedDriver const * const drivers[XEN_UNIFIED_NR_DRIVERS] = {
    [XEN_UNIFIED_HYPERVISOR_OFFSET] = &xenHypervisorDriver,
    [XEN_UNIFIED_XEND_OFFSET] = &xenDaemonDriver,
    [XEN_UNIFIED_XM_OFFSET] = &xenXMDriver,
};

static bool is_privileged = false;

/**
 * xenNumaInit:
 * @conn: pointer to the hypervisor connection
 *
 * Initializer for previous variables. We currently assume that
 * the number of physical CPU and the number of NUMA cell is fixed
 * until reboot which might be false in future Xen implementations.
 */
static void
xenNumaInit(virConnectPtr conn) {
    virNodeInfo nodeInfo;
    xenUnifiedPrivatePtr priv;
    int ret;

    ret = xenUnifiedNodeGetInfo(conn, &nodeInfo);
    if (ret < 0)
        return;

    priv = conn->privateData;

    priv->nbNodeCells = nodeInfo.nodes;
    priv->nbNodeCpus = nodeInfo.cpus;
}


/**
 * xenDomainUsedCpus:
 * @dom: the domain
 *
 * Analyze which set of CPUs are used by the domain and
 * return a string providing the ranges.
 *
 * Returns the string which needs to be freed by the caller or
 *         NULL if the domain uses all CPU or in case of error.
 */
char *
xenDomainUsedCpus(virDomainPtr dom)
{
    char *res = NULL;
    int ncpus;
    int nb_vcpu;
    virBitmapPtr cpulist = NULL;
    unsigned char *cpumap = NULL;
    size_t cpumaplen;
    int nb = 0;
    int n, m;
    virVcpuInfoPtr cpuinfo = NULL;
    virNodeInfo nodeinfo;
    xenUnifiedPrivatePtr priv;

    if (!VIR_IS_CONNECTED_DOMAIN(dom))
        return NULL;

    priv = dom->conn->privateData;

    if (priv->nbNodeCpus <= 0)
        return NULL;
    nb_vcpu = xenUnifiedDomainGetMaxVcpus(dom);
    if (nb_vcpu <= 0)
        return NULL;
    if (xenUnifiedNodeGetInfo(dom->conn, &nodeinfo) < 0)
        return NULL;

    if (!(cpulist = virBitmapNew(priv->nbNodeCpus))) {
        virReportOOMError();
        goto done;
    }
    if (VIR_ALLOC_N(cpuinfo, nb_vcpu) < 0) {
        virReportOOMError();
        goto done;
    }
    cpumaplen = VIR_CPU_MAPLEN(VIR_NODEINFO_MAXCPUS(nodeinfo));
    if (xalloc_oversized(nb_vcpu, cpumaplen) ||
        VIR_ALLOC_N(cpumap, nb_vcpu * cpumaplen) < 0) {
        virReportOOMError();
        goto done;
    }

    if ((ncpus = xenUnifiedDomainGetVcpus(dom, cpuinfo, nb_vcpu,
                                          cpumap, cpumaplen)) >= 0) {
        for (n = 0 ; n < ncpus ; n++) {
            for (m = 0 ; m < priv->nbNodeCpus; m++) {
                bool used;
                ignore_value(virBitmapGetBit(cpulist, m, &used));
                if ((!used) &&
                    (VIR_CPU_USABLE(cpumap, cpumaplen, n, m))) {
                    ignore_value(virBitmapSetBit(cpulist, m));
                    nb++;
                    /* if all CPU are used just return NULL */
                    if (nb == priv->nbNodeCpus)
                        goto done;

                }
            }
        }
        res = virBitmapFormat(cpulist);
    }

done:
    virBitmapFree(cpulist);
    VIR_FREE(cpumap);
    VIR_FREE(cpuinfo);
    return res;
}

static int
xenUnifiedStateInitialize(bool privileged,
                          virStateInhibitCallback callback ATTRIBUTE_UNUSED,
                          void *opaque ATTRIBUTE_UNUSED)
{
    /* Don't allow driver to work in non-root libvirtd */
    if (privileged)
        is_privileged = true;
    return 0;
}

static virStateDriver state_driver = {
    .name = "Xen",
    .stateInitialize = xenUnifiedStateInitialize,
};

/*----- Dispatch functions. -----*/

/* These dispatch functions follow the model used historically
 * by libvirt.c -- trying each low-level Xen driver in turn
 * until one succeeds.  However since we know what low-level
 * drivers can perform which functions, it is probably better
 * in future to optimise these dispatch functions to just call
 * the single function (or small number of appropriate functions)
 * in the low level drivers directly.
 */

static int
xenUnifiedProbe(void)
{
#ifdef __linux__
    if (virFileExists("/proc/xen"))
        return 1;
#endif
#ifdef __sun
    int fd;

    if ((fd = open("/dev/xen/domcaps", O_RDONLY)) >= 0) {
        VIR_FORCE_CLOSE(fd);
        return 1;
    }
#endif
    return 0;
}

#ifdef WITH_LIBXL
static int
xenUnifiedXendProbe(void)
{
    virCommandPtr cmd;
    int status;
    int ret = 0;

    cmd = virCommandNewArgList("/usr/sbin/xend", "status", NULL);
    if (virCommandRun(cmd, &status) == 0 && status == 0)
        ret = 1;
    virCommandFree(cmd);

    return ret;
}
#endif


static int
xenDomainDeviceDefPostParse(virDomainDeviceDefPtr dev,
                            virDomainDefPtr def,
                            virCapsPtr caps ATTRIBUTE_UNUSED,
                            void *opaque ATTRIBUTE_UNUSED)
{
    if (dev->type == VIR_DOMAIN_DEVICE_CHR &&
        dev->data.chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE &&
        dev->data.chr->targetType == VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_NONE &&
        STRNEQ(def->os.type, "hvm"))
        dev->data.chr->targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_XEN;

    return 0;
}


virDomainDefParserConfig xenDomainDefParserConfig = {
    .macPrefix = { 0x00, 0x16, 0x3e },
    .devicesPostParseCallback = xenDomainDeviceDefPostParse,
};


virDomainXMLOptionPtr
xenDomainXMLConfInit(void)
{
    return virDomainXMLOptionNew(&xenDomainDefParserConfig,
                                 NULL, NULL);
}


static virDrvOpenStatus
xenUnifiedConnectOpen(virConnectPtr conn, virConnectAuthPtr auth, unsigned int flags)
{
    xenUnifiedPrivatePtr priv;
    char ebuf[1024];

    /*
     * Only the libvirtd instance can open this driver.
     * Everything else falls back to the remote driver.
     */
    if (!is_privileged)
        return VIR_DRV_OPEN_DECLINED;

    if (conn->uri == NULL) {
        if (!xenUnifiedProbe())
            return VIR_DRV_OPEN_DECLINED;

        if (!(conn->uri = virURIParse("xen:///")))
            return VIR_DRV_OPEN_ERROR;
    } else {
        if (conn->uri->scheme) {
            /* Decline any scheme which isn't "xen://" or "http://". */
            if (STRCASENEQ(conn->uri->scheme, "xen") &&
                STRCASENEQ(conn->uri->scheme, "http"))
                return VIR_DRV_OPEN_DECLINED;


            /* Return an error if the path isn't '' or '/' */
            if (conn->uri->path &&
                STRNEQ(conn->uri->path, "") &&
                STRNEQ(conn->uri->path, "/")) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unexpected Xen URI path '%s', try xen:///"),
                               conn->uri->path);
                return VIR_DRV_OPEN_ERROR;
            }

            /* Decline any xen:// URI with a server specified, allowing remote
             * driver to handle, but keep any http:/// URIs */
            if (STRCASEEQ(conn->uri->scheme, "xen") &&
                conn->uri->server)
                return VIR_DRV_OPEN_DECLINED;
        } else {
            return VIR_DRV_OPEN_DECLINED;
        }
    }

#ifdef WITH_LIBXL
    /* Decline xen:// URI if xend is not running and libxenlight
     * driver is potentially available. */
    if (!xenUnifiedXendProbe())
        return VIR_DRV_OPEN_DECLINED;
#endif

    /* We now know the URI is definitely for this driver, so beyond
     * here, don't return DECLINED, always use ERROR */

    /* Allocate per-connection private data. */
    if (VIR_ALLOC(priv) < 0) {
        virReportOOMError();
        return VIR_DRV_OPEN_ERROR;
    }
    if (virMutexInit(&priv->lock) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("cannot initialize mutex"));
        VIR_FREE(priv);
        return VIR_DRV_OPEN_ERROR;
    }

    if (!(priv->domainEvents = virDomainEventStateNew())) {
        virMutexDestroy(&priv->lock);
        VIR_FREE(priv);
        return VIR_DRV_OPEN_ERROR;
    }
    conn->privateData = priv;

    priv->handle = -1;
    priv->xendConfigVersion = -1;
    priv->xshandle = NULL;


    /* Hypervisor required to succeed */
    VIR_DEBUG("Trying hypervisor sub-driver");
    if (xenHypervisorOpen(conn, auth, flags) < 0)
        goto error;
    VIR_DEBUG("Activated hypervisor sub-driver");
    priv->opened[XEN_UNIFIED_HYPERVISOR_OFFSET] = 1;

    /* XenD is required to succeed */
    VIR_DEBUG("Trying XenD sub-driver");
    if (xenDaemonOpen(conn, auth, flags) < 0)
        goto error;
    VIR_DEBUG("Activated XenD sub-driver");
    priv->opened[XEN_UNIFIED_XEND_OFFSET] = 1;

    /* For old XenD, the XM driver is required to succeed */
    if (priv->xendConfigVersion <= XEND_CONFIG_VERSION_3_0_3) {
        VIR_DEBUG("Trying XM sub-driver");
        if (xenXMOpen(conn, auth, flags) < 0)
            goto error;
        VIR_DEBUG("Activated XM sub-driver");
        priv->opened[XEN_UNIFIED_XM_OFFSET] = 1;
    }

    VIR_DEBUG("Trying XS sub-driver");
    if (xenStoreOpen(conn, auth, flags) < 0)
        goto error;
    VIR_DEBUG("Activated XS sub-driver");
    priv->opened[XEN_UNIFIED_XS_OFFSET] = 1;

    xenNumaInit(conn);

    if (!(priv->caps = xenHypervisorMakeCapabilities(conn))) {
        VIR_DEBUG("Failed to make capabilities");
        goto error;
    }

    if (!(priv->xmlopt = xenDomainXMLConfInit()))
        goto error;

#if WITH_XEN_INOTIFY
    VIR_DEBUG("Trying Xen inotify sub-driver");
    if (xenInotifyOpen(conn, auth, flags) < 0)
        goto error;
    VIR_DEBUG("Activated Xen inotify sub-driver");
    priv->opened[XEN_UNIFIED_INOTIFY_OFFSET] = 1;
#endif

    if (!(priv->saveDir = strdup(XEN_SAVE_DIR))) {
        virReportOOMError();
        goto error;
    }

    if (virFileMakePath(priv->saveDir) < 0) {
        VIR_ERROR(_("Errored to create save dir '%s': %s"), priv->saveDir,
                  virStrerror(errno, ebuf, sizeof(ebuf)));
        goto error;
    }

    return VIR_DRV_OPEN_SUCCESS;

error:
    VIR_DEBUG("Failed to activate a mandatory sub-driver");
#if WITH_XEN_INOTIFY
    if (priv->opened[XEN_UNIFIED_INOTIFY_OFFSET])
        xenInotifyClose(conn);
#endif
    if (priv->opened[XEN_UNIFIED_XM_OFFSET])
        xenXMClose(conn);
    if (priv->opened[XEN_UNIFIED_XS_OFFSET])
        xenStoreClose(conn);
    if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
        xenDaemonClose(conn);
    if (priv->opened[XEN_UNIFIED_HYPERVISOR_OFFSET])
        xenHypervisorClose(conn);
    virMutexDestroy(&priv->lock);
    VIR_FREE(priv->saveDir);
    VIR_FREE(priv);
    conn->privateData = NULL;
    return VIR_DRV_OPEN_ERROR;
}

static int
xenUnifiedConnectClose(virConnectPtr conn)
{
    xenUnifiedPrivatePtr priv = conn->privateData;

    virObjectUnref(priv->caps);
    virObjectUnref(priv->xmlopt);
    virDomainEventStateFree(priv->domainEvents);

#if WITH_XEN_INOTIFY
    if (priv->opened[XEN_UNIFIED_INOTIFY_OFFSET])
        xenInotifyClose(conn);
#endif
    if (priv->opened[XEN_UNIFIED_XM_OFFSET])
        xenXMClose(conn);
    if (priv->opened[XEN_UNIFIED_XS_OFFSET])
        xenStoreClose(conn);
    if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
        xenDaemonClose(conn);
    if (priv->opened[XEN_UNIFIED_HYPERVISOR_OFFSET])
        xenHypervisorClose(conn);

    VIR_FREE(priv->saveDir);
    virMutexDestroy(&priv->lock);
    VIR_FREE(conn->privateData);

    return 0;
}


#define HV_VERSION ((DOM0_INTERFACE_VERSION >> 24) * 1000000 +         \
                    ((DOM0_INTERFACE_VERSION >> 16) & 0xFF) * 1000 +   \
                    (DOM0_INTERFACE_VERSION & 0xFFFF))

unsigned long xenUnifiedVersion(void)
{
    return HV_VERSION;
}


static const char *
xenUnifiedConnectGetType(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return "Xen";
}

/* Which features are supported by this driver? */
static int
xenUnifiedConnectSupportsFeature(virConnectPtr conn ATTRIBUTE_UNUSED, int feature)
{
    switch (feature) {
    case VIR_DRV_FEATURE_MIGRATION_V1:
    case VIR_DRV_FEATURE_MIGRATION_DIRECT:
        return 1;
    default:
        return 0;
    }
}

static int
xenUnifiedConnectGetVersion(virConnectPtr conn, unsigned long *hvVer)
{
    return xenHypervisorGetVersion(conn, hvVer);
}


static char *xenUnifiedConnectGetHostname(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return virGetHostname();
}


static int
xenUnifiedConnectIsEncrypted(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
xenUnifiedConnectIsSecure(virConnectPtr conn)
{
    xenUnifiedPrivatePtr priv = conn->privateData;
    int ret = 1;

    /* All drivers are secure, except for XenD over TCP */
    if (priv->opened[XEN_UNIFIED_XEND_OFFSET] &&
        priv->addrfamily != AF_UNIX)
        ret = 0;

    return ret;
}

static int
xenUnifiedConnectIsAlive(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* XenD reconnects for each request */
    return 1;
}

int
xenUnifiedConnectGetMaxVcpus(virConnectPtr conn, const char *type)
{
    if (type && STRCASENEQ(type, "Xen")) {
        virReportError(VIR_ERR_INVALID_ARG, __FUNCTION__);
        return -1;
    }

    return xenHypervisorGetMaxVcpus(conn, type);
}

static int
xenUnifiedNodeGetInfo(virConnectPtr conn, virNodeInfoPtr info)
{
    return xenDaemonNodeGetInfo(conn, info);
}

static char *
xenUnifiedConnectGetCapabilities(virConnectPtr conn)
{
    xenUnifiedPrivatePtr priv = conn->privateData;
    char *xml;

    if (!(xml = virCapabilitiesFormatXML(priv->caps))) {
        virReportOOMError();
        return NULL;
    }

    return xml;
}

static int
xenUnifiedConnectListDomains(virConnectPtr conn, int *ids, int maxids)
{
    return xenStoreListDomains(conn, ids, maxids);
}

static int
xenUnifiedConnectNumOfDomains(virConnectPtr conn)
{
    return xenStoreNumOfDomains(conn);
}

static virDomainPtr
xenUnifiedDomainCreateXML(virConnectPtr conn,
                          const char *xmlDesc, unsigned int flags)
{
    virCheckFlags(0, NULL);

    return xenDaemonCreateXML(conn, xmlDesc);
}

static virDomainPtr
xenUnifiedDomainLookupByID(virConnectPtr conn, int id)
{
    virDomainPtr ret = NULL;

    ret = xenHypervisorLookupDomainByID(conn, id);

    if (!ret && virGetLastError() == NULL)
        virReportError(VIR_ERR_NO_DOMAIN, __FUNCTION__);

    return ret;
}

static virDomainPtr
xenUnifiedDomainLookupByUUID(virConnectPtr conn,
                             const unsigned char *uuid)
{
    xenUnifiedPrivatePtr priv = conn->privateData;
    virDomainPtr ret;

    ret = xenHypervisorLookupDomainByUUID(conn, uuid);

    /* Try XM for inactive domains. */
    if (!ret) {
        if (priv->xendConfigVersion <= XEND_CONFIG_VERSION_3_0_3)
            ret = xenXMDomainLookupByUUID(conn, uuid);
        else
            ret = xenDaemonLookupByUUID(conn, uuid);
    }

    if (!ret && virGetLastError() == NULL)
        virReportError(VIR_ERR_NO_DOMAIN, __FUNCTION__);

    return ret;
}

static virDomainPtr
xenUnifiedDomainLookupByName(virConnectPtr conn,
                             const char *name)
{
    xenUnifiedPrivatePtr priv = conn->privateData;
    virDomainPtr ret;

    ret = xenDaemonLookupByName(conn, name);

    /* Try XM for inactive domains. */
    if (priv->xendConfigVersion <= XEND_CONFIG_VERSION_3_0_3)
        ret = xenXMDomainLookupByName(conn, name);

    if (!ret && virGetLastError() == NULL)
        virReportError(VIR_ERR_NO_DOMAIN, __FUNCTION__);

    return ret;
}


static int
xenUnifiedDomainIsActive(virDomainPtr dom)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    virDomainPtr currdom;
    int ret = -1;

    /* ID field in dom may be outdated, so re-lookup */
    currdom = xenHypervisorLookupDomainByUUID(dom->conn, dom->uuid);

    /* Try XM for inactive domains. */
    if (!currdom) {
        if (priv->xendConfigVersion <= XEND_CONFIG_VERSION_3_0_3)
            currdom = xenXMDomainLookupByUUID(dom->conn, dom->uuid);
        else
            currdom = xenDaemonLookupByUUID(dom->conn, dom->uuid);
    }

    if (currdom) {
        ret = currdom->id == -1 ? 0 : 1;
        virDomainFree(currdom);
    } else if (virGetLastError() == NULL) {
        virReportError(VIR_ERR_NO_DOMAIN, __FUNCTION__);
    }

    return ret;
}

static int
xenUnifiedDomainIsPersistent(virDomainPtr dom)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    virDomainPtr currdom = NULL;
    int ret = -1;

    if (priv->opened[XEN_UNIFIED_XM_OFFSET]) {
        /* Old Xen, pre-inactive domain management.
         * If the XM driver can see the guest, it is definitely persistent */
        currdom = xenXMDomainLookupByUUID(dom->conn, dom->uuid);
        if (currdom)
            ret = 1;
        else
            ret = 0;
    } else {
        /* New Xen with inactive domain management */
        currdom = xenDaemonLookupByUUID(dom->conn, dom->uuid);
        if (currdom) {
            if (currdom->id == -1) {
                /* If its inactive, then trivially, it must be persistent */
                ret = 1;
            } else {
                char *path;
                char uuidstr[VIR_UUID_STRING_BUFLEN];

                /* If its running there's no official way to tell, so we
                 * go behind xend's back & look at the config dir */
                virUUIDFormat(dom->uuid, uuidstr);
                if (virAsprintf(&path, "%s/%s", XEND_DOMAINS_DIR, uuidstr) < 0) {
                    virReportOOMError();
                    goto done;
                }
                if (access(path, R_OK) == 0)
                    ret = 1;
                else if (errno == ENOENT)
                    ret = 0;
            }
        }
    }

done:
    if (currdom)
        virDomainFree(currdom);

    return ret;
}

static int
xenUnifiedDomainIsUpdated(virDomainPtr dom ATTRIBUTE_UNUSED)
{
    return 0;
}

static int
xenUnifiedDomainSuspend(virDomainPtr dom)
{
    return xenDaemonDomainSuspend(dom);
}

static int
xenUnifiedDomainResume(virDomainPtr dom)
{
    return xenDaemonDomainResume(dom);
}

static int
xenUnifiedDomainShutdownFlags(virDomainPtr dom,
                              unsigned int flags)
{
    virCheckFlags(0, -1);

    return xenDaemonDomainShutdown(dom);
}

static int
xenUnifiedDomainShutdown(virDomainPtr dom)
{
    return xenUnifiedDomainShutdownFlags(dom, 0);
}

static int
xenUnifiedDomainReboot(virDomainPtr dom, unsigned int flags)
{
    virCheckFlags(0, -1);

    return xenDaemonDomainReboot(dom);
}

static int
xenUnifiedDomainDestroyFlags(virDomainPtr dom,
                             unsigned int flags)
{
    virCheckFlags(0, -1);

    return xenDaemonDomainDestroy(dom);
}

static int
xenUnifiedDomainDestroy(virDomainPtr dom)
{
    return xenUnifiedDomainDestroyFlags(dom, 0);
}

static char *
xenUnifiedDomainGetOSType(virDomainPtr dom)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (dom->id < 0) {
        if (priv->xendConfigVersion < XEND_CONFIG_VERSION_3_0_4) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unable to query OS type for inactive domain"));
            return NULL;
        } else {
            return xenDaemonDomainGetOSType(dom);
        }
    } else {
        return xenHypervisorDomainGetOSType(dom);
    }
}


static unsigned long long
xenUnifiedDomainGetMaxMemory(virDomainPtr dom)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (dom->id < 0) {
        if (priv->xendConfigVersion < XEND_CONFIG_VERSION_3_0_4)
            return xenXMDomainGetMaxMemory(dom);
        else
            return xenDaemonDomainGetMaxMemory(dom);
    } else {
        return xenHypervisorGetMaxMemory(dom);
    }
}

static int
xenUnifiedDomainSetMaxMemory(virDomainPtr dom, unsigned long memory)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (dom->id < 0) {
        if (priv->xendConfigVersion < XEND_CONFIG_VERSION_3_0_4)
            return xenXMDomainSetMaxMemory(dom, memory);
        else
            return xenDaemonDomainSetMaxMemory(dom, memory);
    } else {
        return xenHypervisorSetMaxMemory(dom, memory);
    }
}

static int
xenUnifiedDomainSetMemory(virDomainPtr dom, unsigned long memory)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (dom->id < 0 && priv->xendConfigVersion < XEND_CONFIG_VERSION_3_0_4)
        return xenXMDomainSetMemory(dom, memory);
    else
        return xenDaemonDomainSetMemory(dom, memory);
}

static int
xenUnifiedDomainGetInfo(virDomainPtr dom, virDomainInfoPtr info)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (dom->id < 0) {
        if (priv->xendConfigVersion < XEND_CONFIG_VERSION_3_0_4)
            return xenXMDomainGetInfo(dom, info);
        else
            return xenDaemonDomainGetInfo(dom, info);
    } else {
        return xenHypervisorGetDomainInfo(dom, info);
    }
}

static int
xenUnifiedDomainGetState(virDomainPtr dom,
                         int *state,
                         int *reason,
                         unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    virCheckFlags(0, -1);

    if (dom->id < 0) {
        if (priv->xendConfigVersion < XEND_CONFIG_VERSION_3_0_4)
            return xenXMDomainGetState(dom, state, reason);
        else
            return xenDaemonDomainGetState(dom, state, reason);
    } else {
        return xenHypervisorGetDomainState(dom, state, reason);
    }
}

static int
xenUnifiedDomainSaveFlags(virDomainPtr dom, const char *to, const char *dxml,
                          unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    virCheckFlags(0, -1);
    if (dxml) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("xml modification unsupported"));
        return -1;
    }

    if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
        return xenDaemonDomainSave(dom, to);
    return -1;
}

static int
xenUnifiedDomainSave(virDomainPtr dom, const char *to)
{
    return xenUnifiedDomainSaveFlags(dom, to, NULL, 0);
}

static char *
xenUnifiedDomainManagedSavePath(xenUnifiedPrivatePtr priv, virDomainPtr dom)
{
    char *ret;

    if (virAsprintf(&ret, "%s/%s.save", priv->saveDir, dom->name) < 0) {
        virReportOOMError();
        return NULL;
    }

    VIR_DEBUG("managed save image: %s", ret);
    return ret;
}

static int
xenUnifiedDomainManagedSave(virDomainPtr dom, unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    char *name;
    int ret = -1;

    virCheckFlags(0, -1);

    name = xenUnifiedDomainManagedSavePath(priv, dom);
    if (!name)
        goto cleanup;

    if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
        ret = xenDaemonDomainSave(dom, name);

cleanup:
    VIR_FREE(name);
    return ret;
}

static int
xenUnifiedDomainHasManagedSaveImage(virDomainPtr dom, unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    char *name;
    int ret = -1;

    virCheckFlags(0, -1);

    name = xenUnifiedDomainManagedSavePath(priv, dom);
    if (!name)
        return ret;

    ret = virFileExists(name);
    VIR_FREE(name);
    return ret;
}

static int
xenUnifiedDomainManagedSaveRemove(virDomainPtr dom, unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    char *name;
    int ret = -1;

    virCheckFlags(0, -1);

    name = xenUnifiedDomainManagedSavePath(priv, dom);
    if (!name)
        return ret;

    ret = unlink(name);
    VIR_FREE(name);
    return ret;
}

static int
xenUnifiedDomainRestoreFlags(virConnectPtr conn, const char *from,
                             const char *dxml, unsigned int flags)
{
    xenUnifiedPrivatePtr priv = conn->privateData;

    virCheckFlags(0, -1);
    if (dxml) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("xml modification unsupported"));
        return -1;
    }

    if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
        return xenDaemonDomainRestore(conn, from);
    return -1;
}

static int
xenUnifiedDomainRestore(virConnectPtr conn, const char *from)
{
    return xenUnifiedDomainRestoreFlags(conn, from, NULL, 0);
}

static int
xenUnifiedDomainCoreDump(virDomainPtr dom, const char *to, unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
        return xenDaemonDomainCoreDump(dom, to, flags);
    return -1;
}

static int
xenUnifiedDomainSetVcpusFlags(virDomainPtr dom, unsigned int nvcpus,
                              unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int ret;

    virCheckFlags(VIR_DOMAIN_VCPU_LIVE |
                  VIR_DOMAIN_VCPU_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM, -1);

    /* At least one of LIVE or CONFIG must be set.  MAXIMUM cannot be
     * mixed with LIVE.  */
    if ((flags & (VIR_DOMAIN_VCPU_LIVE | VIR_DOMAIN_VCPU_CONFIG)) == 0 ||
        (flags & (VIR_DOMAIN_VCPU_MAXIMUM | VIR_DOMAIN_VCPU_LIVE)) ==
         (VIR_DOMAIN_VCPU_MAXIMUM | VIR_DOMAIN_VCPU_LIVE)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("invalid flag combination: (0x%x)"), flags);
        return -1;
    }
    if (!nvcpus || (unsigned short) nvcpus != nvcpus) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("argument out of range: %d"), nvcpus);
        return -1;
    }

    /* Try non-hypervisor methods first, then hypervisor direct method
     * as a last resort.
     */
    if (priv->opened[XEN_UNIFIED_XEND_OFFSET]) {
        ret = xenDaemonDomainSetVcpusFlags(dom, nvcpus, flags);
        if (ret != -2)
            return ret;
    }
    if (priv->opened[XEN_UNIFIED_XM_OFFSET]) {
        ret = xenXMDomainSetVcpusFlags(dom, nvcpus, flags);
        if (ret != -2)
            return ret;
    }
    if (flags == VIR_DOMAIN_VCPU_LIVE)
        return xenHypervisorSetVcpus(dom, nvcpus);

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static int
xenUnifiedDomainSetVcpus(virDomainPtr dom, unsigned int nvcpus)
{
    unsigned int flags = VIR_DOMAIN_VCPU_LIVE;

    /* Per the documented API, it is hypervisor-dependent whether this
     * affects just _LIVE or _LIVE|_CONFIG; in xen's case, that
     * depends on xendConfigVersion.  */
    if (dom) {
        xenUnifiedPrivatePtr priv = dom->conn->privateData;
        if (priv->xendConfigVersion >= XEND_CONFIG_VERSION_3_0_4)
            flags |= VIR_DOMAIN_VCPU_CONFIG;
        return xenUnifiedDomainSetVcpusFlags(dom, nvcpus, flags);
    }
    return -1;
}

static int
xenUnifiedDomainPinVcpu(virDomainPtr dom, unsigned int vcpu,
                        unsigned char *cpumap, int maplen)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] &&
            drivers[i]->xenDomainPinVcpu &&
            drivers[i]->xenDomainPinVcpu(dom, vcpu, cpumap, maplen) == 0)
            return 0;

    return -1;
}

static int
xenUnifiedDomainGetVcpus(virDomainPtr dom,
                         virVcpuInfoPtr info, int maxinfo,
                         unsigned char *cpumaps, int maplen)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i, ret;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] && drivers[i]->xenDomainGetVcpus) {
            ret = drivers[i]->xenDomainGetVcpus(dom, info, maxinfo, cpumaps, maplen);
            if (ret > 0)
                return ret;
        }
    return -1;
}

static int
xenUnifiedDomainGetVcpusFlags(virDomainPtr dom, unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int ret;

    virCheckFlags(VIR_DOMAIN_VCPU_LIVE |
                  VIR_DOMAIN_VCPU_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM, -1);

    if (priv->opened[XEN_UNIFIED_XEND_OFFSET]) {
        ret = xenDaemonDomainGetVcpusFlags(dom, flags);
        if (ret != -2)
            return ret;
    }
    if (priv->opened[XEN_UNIFIED_XM_OFFSET]) {
        ret = xenXMDomainGetVcpusFlags(dom, flags);
        if (ret != -2)
            return ret;
    }
    if (flags == (VIR_DOMAIN_VCPU_CONFIG | VIR_DOMAIN_VCPU_MAXIMUM))
        return xenHypervisorGetVcpuMax(dom);

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static int
xenUnifiedDomainGetMaxVcpus(virDomainPtr dom)
{
    return xenUnifiedDomainGetVcpusFlags(dom, (VIR_DOMAIN_VCPU_LIVE |
                                               VIR_DOMAIN_VCPU_MAXIMUM));
}

static char *
xenUnifiedDomainGetXMLDesc(virDomainPtr dom, unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (dom->id == -1 && priv->xendConfigVersion < XEND_CONFIG_VERSION_3_0_4) {
        if (priv->opened[XEN_UNIFIED_XM_OFFSET])
            return xenXMDomainGetXMLDesc(dom, flags);
    } else {
        if (priv->opened[XEN_UNIFIED_XEND_OFFSET]) {
            char *cpus, *res;
            xenUnifiedLock(priv);
            cpus = xenDomainUsedCpus(dom);
            xenUnifiedUnlock(priv);
            res = xenDaemonDomainGetXMLDesc(dom, flags, cpus);
            VIR_FREE(cpus);
            return res;
        }
    }

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return NULL;
}


static char *
xenUnifiedConnectDomainXMLFromNative(virConnectPtr conn,
                                     const char *format,
                                     const char *config,
                                     unsigned int flags)
{
    virDomainDefPtr def = NULL;
    char *ret = NULL;
    virConfPtr conf = NULL;
    int id;
    char * tty;
    int vncport;
    xenUnifiedPrivatePtr priv = conn->privateData;

    virCheckFlags(0, NULL);

    if (STRNEQ(format, XEN_CONFIG_FORMAT_XM) &&
        STRNEQ(format, XEN_CONFIG_FORMAT_SEXPR)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("unsupported config type %s"), format);
        return NULL;
    }

    if (STREQ(format, XEN_CONFIG_FORMAT_XM)) {
        conf = virConfReadMem(config, strlen(config), 0);
        if (!conf)
            goto cleanup;

        def = xenParseXM(conf, priv->xendConfigVersion, priv->caps);
    } else if (STREQ(format, XEN_CONFIG_FORMAT_SEXPR)) {
        id = xenGetDomIdFromSxprString(config, priv->xendConfigVersion);
        xenUnifiedLock(priv);
        tty = xenStoreDomainGetConsolePath(conn, id);
        vncport = xenStoreDomainGetVNCPort(conn, id);
        xenUnifiedUnlock(priv);
        def = xenParseSxprString(config, priv->xendConfigVersion, tty,
                                       vncport);
    }
    if (!def)
        goto cleanup;

    ret = virDomainDefFormat(def, 0);

cleanup:
    virDomainDefFree(def);
    if (conf)
        virConfFree(conf);
    return ret;
}


#define MAX_CONFIG_SIZE (1024 * 65)
static char *
xenUnifiedConnectDomainXMLToNative(virConnectPtr conn,
                                   const char *format,
                                   const char *xmlData,
                                   unsigned int flags)
{
    virDomainDefPtr def = NULL;
    char *ret = NULL;
    virConfPtr conf = NULL;
    xenUnifiedPrivatePtr priv = conn->privateData;

    virCheckFlags(0, NULL);

    if (STRNEQ(format, XEN_CONFIG_FORMAT_XM) &&
        STRNEQ(format, XEN_CONFIG_FORMAT_SEXPR)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("unsupported config type %s"), format);
        goto cleanup;
    }

    if (!(def = virDomainDefParseString(xmlData, priv->caps, priv->xmlopt,
                                        1 << VIR_DOMAIN_VIRT_XEN, 0)))
        goto cleanup;

    if (STREQ(format, XEN_CONFIG_FORMAT_XM)) {
        int len = MAX_CONFIG_SIZE;
        conf = xenFormatXM(conn, def, priv->xendConfigVersion);
        if (!conf)
            goto cleanup;

        if (VIR_ALLOC_N(ret, len) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        if (virConfWriteMem(ret, &len, conf) < 0) {
            VIR_FREE(ret);
            goto cleanup;
        }
    } else if (STREQ(format, XEN_CONFIG_FORMAT_SEXPR)) {
        ret = xenFormatSxpr(conn, def, priv->xendConfigVersion);
    }

cleanup:
    virDomainDefFree(def);
    if (conf)
        virConfFree(conf);
    return ret;
}


static int
xenUnifiedDomainMigratePrepare(virConnectPtr dconn,
                               char **cookie,
                               int *cookielen,
                               const char *uri_in,
                               char **uri_out,
                               unsigned long flags,
                               const char *dname,
                               unsigned long resource)
{
    xenUnifiedPrivatePtr priv = dconn->privateData;

    virCheckFlags(XEN_MIGRATION_FLAGS, -1);

    if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
        return xenDaemonDomainMigratePrepare(dconn, cookie, cookielen,
                                             uri_in, uri_out,
                                             flags, dname, resource);

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static int
xenUnifiedDomainMigratePerform(virDomainPtr dom,
                               const char *cookie,
                               int cookielen,
                               const char *uri,
                               unsigned long flags,
                               const char *dname,
                               unsigned long resource)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    virCheckFlags(XEN_MIGRATION_FLAGS, -1);

    if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
        return xenDaemonDomainMigratePerform(dom, cookie, cookielen, uri,
                                             flags, dname, resource);

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static virDomainPtr
xenUnifiedDomainMigrateFinish(virConnectPtr dconn,
                              const char *dname,
                              const char *cookie ATTRIBUTE_UNUSED,
                              int cookielen ATTRIBUTE_UNUSED,
                              const char *uri ATTRIBUTE_UNUSED,
                              unsigned long flags)
{
    virDomainPtr dom = NULL;
    char *domain_xml = NULL;
    virDomainPtr dom_new = NULL;

    virCheckFlags(XEN_MIGRATION_FLAGS, NULL);

    dom = xenUnifiedDomainLookupByName(dconn, dname);
    if (! dom) {
        return NULL;
    }

    if (flags & VIR_MIGRATE_PERSIST_DEST) {
        domain_xml = xenDaemonDomainGetXMLDesc(dom, 0, NULL);
        if (! domain_xml) {
            virReportError(VIR_ERR_MIGRATE_PERSIST_FAILED,
                           "%s", _("failed to get XML representation of migrated domain"));
            goto failure;
        }

        dom_new = xenDaemonDomainDefineXML(dconn, domain_xml);
        if (! dom_new) {
            virReportError(VIR_ERR_MIGRATE_PERSIST_FAILED,
                           "%s", _("failed to define domain on destination host"));
            goto failure;
        }

        /* Free additional reference added by Define */
        virDomainFree(dom_new);
    }

    VIR_FREE(domain_xml);

    return dom;


failure:
    virDomainFree(dom);

    VIR_FREE(domain_xml);

    return NULL;
}

static int
xenUnifiedConnectListDefinedDomains(virConnectPtr conn, char **const names,
                                    int maxnames)
{
    xenUnifiedPrivatePtr priv = conn->privateData;
    int i;
    int ret;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] && drivers[i]->xenListDefinedDomains) {
            ret = drivers[i]->xenListDefinedDomains(conn, names, maxnames);
            if (ret >= 0) return ret;
        }

    return -1;
}

static int
xenUnifiedConnectNumOfDefinedDomains(virConnectPtr conn)
{
    xenUnifiedPrivatePtr priv = conn->privateData;
    int i;
    int ret;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] && drivers[i]->xenNumOfDefinedDomains) {
            ret = drivers[i]->xenNumOfDefinedDomains(conn);
            if (ret >= 0) return ret;
        }

    return -1;
}

static int
xenUnifiedDomainCreateWithFlags(virDomainPtr dom, unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i;
    int ret = -1;
    char *name = NULL;

    virCheckFlags(0, -1);

    name = xenUnifiedDomainManagedSavePath(priv, dom);
    if (!name)
        goto cleanup;

    if (virFileExists(name)) {
        if (priv->opened[XEN_UNIFIED_XEND_OFFSET]) {
            ret = xenDaemonDomainRestore(dom->conn, name);
            if (ret == 0)
                unlink(name);
        }
        goto cleanup;
    }

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i) {
        if (priv->opened[i] && drivers[i]->xenDomainCreate &&
            drivers[i]->xenDomainCreate(dom) == 0) {
            ret = 0;
            goto cleanup;
        }
    }

cleanup:
    VIR_FREE(name);
    return ret;
}

static int
xenUnifiedDomainCreate(virDomainPtr dom)
{
    return xenUnifiedDomainCreateWithFlags(dom, 0);
}

static virDomainPtr
xenUnifiedDomainDefineXML(virConnectPtr conn, const char *xml)
{
    xenUnifiedPrivatePtr priv = conn->privateData;
    int i;
    virDomainPtr ret;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] && drivers[i]->xenDomainDefineXML) {
            ret = drivers[i]->xenDomainDefineXML(conn, xml);
            if (ret) return ret;
        }

    return NULL;
}

static int
xenUnifiedDomainUndefineFlags(virDomainPtr dom, unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i;

    virCheckFlags(0, -1);
    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] && drivers[i]->xenDomainUndefine &&
            drivers[i]->xenDomainUndefine(dom) == 0)
            return 0;

    return -1;
}

static int
xenUnifiedDomainUndefine(virDomainPtr dom) {
    return xenUnifiedDomainUndefineFlags(dom, 0);
}

static int
xenUnifiedDomainAttachDevice(virDomainPtr dom, const char *xml)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i;
    unsigned int flags = VIR_DOMAIN_DEVICE_MODIFY_LIVE;

    /*
     * HACK: xend with xendConfigVersion >= 3 does not support changing live
     * config without touching persistent config, we add the extra flag here
     * to make this API work
     */
    if (priv->opened[XEN_UNIFIED_XEND_OFFSET] &&
        priv->xendConfigVersion >= XEND_CONFIG_VERSION_3_0_4)
        flags |= VIR_DOMAIN_DEVICE_MODIFY_CONFIG;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] && drivers[i]->xenDomainAttachDeviceFlags &&
            drivers[i]->xenDomainAttachDeviceFlags(dom, xml, flags) == 0)
            return 0;

    return -1;
}

static int
xenUnifiedDomainAttachDeviceFlags(virDomainPtr dom, const char *xml,
                                  unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] && drivers[i]->xenDomainAttachDeviceFlags &&
            drivers[i]->xenDomainAttachDeviceFlags(dom, xml, flags) == 0)
            return 0;

    return -1;
}

static int
xenUnifiedDomainDetachDevice(virDomainPtr dom, const char *xml)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i;
    unsigned int flags = VIR_DOMAIN_DEVICE_MODIFY_LIVE;

    /*
     * HACK: xend with xendConfigVersion >= 3 does not support changing live
     * config without touching persistent config, we add the extra flag here
     * to make this API work
     */
    if (priv->opened[XEN_UNIFIED_XEND_OFFSET] &&
        priv->xendConfigVersion >= XEND_CONFIG_VERSION_3_0_4)
        flags |= VIR_DOMAIN_DEVICE_MODIFY_CONFIG;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] && drivers[i]->xenDomainDetachDeviceFlags &&
            drivers[i]->xenDomainDetachDeviceFlags(dom, xml, flags) == 0)
            return 0;

    return -1;
}

static int
xenUnifiedDomainDetachDeviceFlags(virDomainPtr dom, const char *xml,
                                  unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i)
        if (priv->opened[i] && drivers[i]->xenDomainDetachDeviceFlags &&
            drivers[i]->xenDomainDetachDeviceFlags(dom, xml, flags) == 0)
            return 0;

    return -1;
}

static int
xenUnifiedDomainUpdateDeviceFlags(virDomainPtr dom, const char *xml,
                                  unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
        return xenDaemonUpdateDeviceFlags(dom, xml, flags);
    return -1;
}

static int
xenUnifiedDomainGetAutostart(virDomainPtr dom, int *autostart)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (priv->xendConfigVersion < XEND_CONFIG_VERSION_3_0_4) {
        if (priv->opened[XEN_UNIFIED_XM_OFFSET])
            return xenXMDomainGetAutostart(dom, autostart);
    } else {
        if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
            return xenDaemonDomainGetAutostart(dom, autostart);
    }

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static int
xenUnifiedDomainSetAutostart(virDomainPtr dom, int autostart)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (priv->xendConfigVersion < XEND_CONFIG_VERSION_3_0_4) {
        if (priv->opened[XEN_UNIFIED_XM_OFFSET])
            return xenXMDomainSetAutostart(dom, autostart);
    } else {
        if (priv->opened[XEN_UNIFIED_XEND_OFFSET])
            return xenDaemonDomainSetAutostart(dom, autostart);
    }

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static char *
xenUnifiedDomainGetSchedulerType(virDomainPtr dom, int *nparams)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i;
    char *schedulertype;

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; i++) {
        if (priv->opened[i] && drivers[i]->xenDomainGetSchedulerType) {
            schedulertype = drivers[i]->xenDomainGetSchedulerType(dom, nparams);
            if (schedulertype != NULL)
                return schedulertype;
        }
    }
    return NULL;
}

static int
xenUnifiedDomainGetSchedulerParametersFlags(virDomainPtr dom,
                                            virTypedParameterPtr params,
                                            int *nparams,
                                            unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i, ret;

    virCheckFlags(0, -1);

    for (i = 0; i < XEN_UNIFIED_NR_DRIVERS; ++i) {
        if (priv->opened[i] && drivers[i]->xenDomainGetSchedulerParameters) {
           ret = drivers[i]->xenDomainGetSchedulerParameters(dom, params, nparams);
           if (ret == 0)
               return 0;
        }
    }
    return -1;
}

static int
xenUnifiedDomainGetSchedulerParameters(virDomainPtr dom,
                                       virTypedParameterPtr params,
                                       int *nparams)
{
    return xenUnifiedDomainGetSchedulerParametersFlags(dom, params,
                                                       nparams, 0);
}

static int
xenUnifiedDomainSetSchedulerParametersFlags(virDomainPtr dom,
                                            virTypedParameterPtr params,
                                            int nparams,
                                            unsigned int flags)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;
    int i, ret;

    virCheckFlags(0, -1);

    /* do the hypervisor call last to get better error */
    for (i = XEN_UNIFIED_NR_DRIVERS - 1; i >= 0; i--) {
        if (priv->opened[i] && drivers[i]->xenDomainSetSchedulerParameters) {
           ret = drivers[i]->xenDomainSetSchedulerParameters(dom, params, nparams);
           if (ret == 0)
               return 0;
        }
    }

    return -1;
}

static int
xenUnifiedDomainSetSchedulerParameters(virDomainPtr dom,
                                       virTypedParameterPtr params,
                                       int nparams)
{
    return xenUnifiedDomainSetSchedulerParametersFlags(dom, params,
                                                       nparams, 0);
}

static int
xenUnifiedDomainBlockStats(virDomainPtr dom, const char *path,
                           struct _virDomainBlockStats *stats)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (priv->opened[XEN_UNIFIED_HYPERVISOR_OFFSET])
        return xenHypervisorDomainBlockStats(dom, path, stats);

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static int
xenUnifiedDomainInterfaceStats(virDomainPtr dom, const char *path,
                               struct _virDomainInterfaceStats *stats)
{
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    if (priv->opened[XEN_UNIFIED_HYPERVISOR_OFFSET])
        return xenHypervisorDomainInterfaceStats(dom, path, stats);

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static int
xenUnifiedDomainBlockPeek(virDomainPtr dom, const char *path,
                          unsigned long long offset, size_t size,
                          void *buffer, unsigned int flags)
{
    int r;
    xenUnifiedPrivatePtr priv = dom->conn->privateData;

    virCheckFlags(0, -1);

    if (priv->opened[XEN_UNIFIED_XEND_OFFSET]) {
        r = xenDaemonDomainBlockPeek(dom, path, offset, size, buffer);
        if (r != -2) return r;
        /* r == -2 means declined, so fall through to XM driver ... */
    }

    if (priv->opened[XEN_UNIFIED_XM_OFFSET]) {
        if (xenXMDomainBlockPeek(dom, path, offset, size, buffer) == 0)
            return 0;
    }

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static int
xenUnifiedNodeGetCellsFreeMemory(virConnectPtr conn, unsigned long long *freeMems,
                                 int startCell, int maxCells)
{
    xenUnifiedPrivatePtr priv = conn->privateData;

    if (priv->opened[XEN_UNIFIED_HYPERVISOR_OFFSET])
        return xenHypervisorNodeGetCellsFreeMemory(conn, freeMems,
                                                   startCell, maxCells);

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return -1;
}

static unsigned long long
xenUnifiedNodeGetFreeMemory(virConnectPtr conn)
{
    unsigned long long freeMem = 0;
    int ret;
    xenUnifiedPrivatePtr priv = conn->privateData;

    if (priv->opened[XEN_UNIFIED_HYPERVISOR_OFFSET]) {
        ret = xenHypervisorNodeGetCellsFreeMemory(conn, &freeMem,
                                                  -1, 1);
        if (ret != 1)
            return 0;
        return freeMem;
    }

    virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
    return 0;
}


static int
xenUnifiedConnectDomainEventRegister(virConnectPtr conn,
                                     virConnectDomainEventCallback callback,
                                     void *opaque,
                                     virFreeCallback freefunc)
{
    xenUnifiedPrivatePtr priv = conn->privateData;

    int ret;
    xenUnifiedLock(priv);

    if (priv->xsWatch == -1) {
        virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
        xenUnifiedUnlock(priv);
        return -1;
    }

    ret = virDomainEventStateRegister(conn, priv->domainEvents,
                                      callback, opaque, freefunc);

    xenUnifiedUnlock(priv);
    return ret;
}


static int
xenUnifiedConnectDomainEventDeregister(virConnectPtr conn,
                                       virConnectDomainEventCallback callback)
{
    int ret;
    xenUnifiedPrivatePtr priv = conn->privateData;
    xenUnifiedLock(priv);

    if (priv->xsWatch == -1) {
        virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
        xenUnifiedUnlock(priv);
        return -1;
    }

    ret = virDomainEventStateDeregister(conn,
                                        priv->domainEvents,
                                        callback);

    xenUnifiedUnlock(priv);
    return ret;
}


static int
xenUnifiedConnectDomainEventRegisterAny(virConnectPtr conn,
                                        virDomainPtr dom,
                                        int eventID,
                                        virConnectDomainEventGenericCallback callback,
                                        void *opaque,
                                        virFreeCallback freefunc)
{
    xenUnifiedPrivatePtr priv = conn->privateData;

    int ret;
    xenUnifiedLock(priv);

    if (priv->xsWatch == -1) {
        virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
        xenUnifiedUnlock(priv);
        return -1;
    }

    if (virDomainEventStateRegisterID(conn, priv->domainEvents,
                                      dom, eventID,
                                      callback, opaque, freefunc, &ret) < 0)
        ret = -1;

    xenUnifiedUnlock(priv);
    return ret;
}

static int
xenUnifiedConnectDomainEventDeregisterAny(virConnectPtr conn,
                                          int callbackID)
{
    int ret;
    xenUnifiedPrivatePtr priv = conn->privateData;
    xenUnifiedLock(priv);

    if (priv->xsWatch == -1) {
        virReportError(VIR_ERR_NO_SUPPORT, __FUNCTION__);
        xenUnifiedUnlock(priv);
        return -1;
    }

    ret = virDomainEventStateDeregisterID(conn,
                                          priv->domainEvents,
                                          callbackID);

    xenUnifiedUnlock(priv);
    return ret;
}


static int
xenUnifiedNodeDeviceGetPciInfo(virNodeDevicePtr dev,
                               unsigned *domain,
                               unsigned *bus,
                               unsigned *slot,
                               unsigned *function)
{
    virNodeDeviceDefPtr def = NULL;
    virNodeDevCapsDefPtr cap;
    char *xml = NULL;
    int ret = -1;

    xml = virNodeDeviceGetXMLDesc(dev, 0);
    if (!xml)
        goto out;

    def = virNodeDeviceDefParseString(xml, EXISTING_DEVICE, NULL);
    if (!def)
        goto out;

    cap = def->caps;
    while (cap) {
        if (cap->type == VIR_NODE_DEV_CAP_PCI_DEV) {
            *domain   = cap->data.pci_dev.domain;
            *bus      = cap->data.pci_dev.bus;
            *slot     = cap->data.pci_dev.slot;
            *function = cap->data.pci_dev.function;
            break;
        }

        cap = cap->next;
    }

    if (!cap) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("device %s is not a PCI device"), dev->name);
        goto out;
    }

    ret = 0;
out:
    virNodeDeviceDefFree(def);
    VIR_FREE(xml);
    return ret;
}

static int
xenUnifiedNodeDeviceDetachFlags(virNodeDevicePtr dev,
                                const char *driverName,
                                unsigned int flags)
{
    virPCIDevicePtr pci;
    unsigned domain, bus, slot, function;
    int ret = -1;

    virCheckFlags(0, -1);

    if (xenUnifiedNodeDeviceGetPciInfo(dev, &domain, &bus, &slot, &function) < 0)
        return -1;

    pci = virPCIDeviceNew(domain, bus, slot, function);
    if (!pci)
        return -1;

    if (!driverName) {
        virPCIDeviceSetStubDriver(pci, "pciback");
    } else {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("unknown driver name '%s'"), driverName);
        goto out;
    }

    if (virPCIDeviceDetach(pci, NULL, NULL, NULL) < 0)
        goto out;

    ret = 0;
out:
    virPCIDeviceFree(pci);
    return ret;
}

static int
xenUnifiedNodeDeviceDettach(virNodeDevicePtr dev)
{
    return xenUnifiedNodeDeviceDetachFlags(dev, NULL, 0);
}

static int
xenUnifiedNodeDeviceAssignedDomainId(virNodeDevicePtr dev)
{
    int numdomains;
    int ret = -1, i;
    int *ids = NULL;
    char *bdf = NULL;
    char *xref = NULL;
    unsigned int domain, bus, slot, function;
    virConnectPtr conn = dev->conn;
    xenUnifiedPrivatePtr priv = conn->privateData;

    /* Get active domains */
    numdomains = xenUnifiedConnectNumOfDomains(conn);
    if (numdomains < 0) {
        return ret;
    }
    if (numdomains > 0){
        if (VIR_ALLOC_N(ids, numdomains) < 0) {
            virReportOOMError();
            goto out;
        }
        if ((numdomains = xenUnifiedConnectListDomains(conn, &ids[0], numdomains)) < 0) {
            goto out;
        }
    }

    /* Get pci bdf */
    if (xenUnifiedNodeDeviceGetPciInfo(dev, &domain, &bus, &slot, &function) < 0)
        goto out;

    if (virAsprintf(&bdf, "%04x:%02x:%02x.%0x",
                    domain, bus, slot, function) < 0) {
        virReportOOMError();
        goto out;
    }

    xenUnifiedLock(priv);
    /* Check if bdf is assigned to one of active domains */
    for (i = 0; i < numdomains; i++) {
        xref = xenStoreDomainGetPCIID(conn, ids[i], bdf);
        if (xref == NULL) {
            continue;
        } else {
            ret = ids[i];
            break;
        }
    }
    xenUnifiedUnlock(priv);

    VIR_FREE(xref);
    VIR_FREE(bdf);
out:
    VIR_FREE(ids);

    return ret;
}

static int
xenUnifiedNodeDeviceReAttach(virNodeDevicePtr dev)
{
    virPCIDevicePtr pci;
    unsigned domain, bus, slot, function;
    int ret = -1;
    int domid;

    if (xenUnifiedNodeDeviceGetPciInfo(dev, &domain, &bus, &slot, &function) < 0)
        return -1;

    pci = virPCIDeviceNew(domain, bus, slot, function);
    if (!pci)
        return -1;

    /* Check if device is assigned to an active guest */
    if ((domid = xenUnifiedNodeDeviceAssignedDomainId(dev)) >= 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Device %s has been assigned to guest %d"),
                       dev->name, domid);
        goto out;
    }

    if (virPCIDeviceReattach(pci, NULL, NULL) < 0)
        goto out;

    ret = 0;
out:
    virPCIDeviceFree(pci);
    return ret;
}

static int
xenUnifiedNodeDeviceReset(virNodeDevicePtr dev)
{
    virPCIDevicePtr pci;
    unsigned domain, bus, slot, function;
    int ret = -1;

    if (xenUnifiedNodeDeviceGetPciInfo(dev, &domain, &bus, &slot, &function) < 0)
        return -1;

    pci = virPCIDeviceNew(domain, bus, slot, function);
    if (!pci)
        return -1;

    if (virPCIDeviceReset(pci, NULL, NULL) < 0)
        goto out;

    ret = 0;
out:
    virPCIDeviceFree(pci);
    return ret;
}


static int
xenUnifiedDomainOpenConsole(virDomainPtr dom,
                            const char *dev_name,
                            virStreamPtr st,
                            unsigned int flags)
{
    virDomainDefPtr def = NULL;
    int ret = -1;
    virDomainChrDefPtr chr = NULL;

    virCheckFlags(0, -1);

    if (dom->id == -1) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("domain is not running"));
        goto cleanup;
    }

    if (dev_name) {
        /* XXX support device aliases in future */
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Named device aliases are not supported"));
        goto cleanup;
    }

    def = xenDaemonDomainFetch(dom->conn, dom->id, dom->name, NULL);
    if (!def)
        goto cleanup;

    if (def->nconsoles)
        chr = def->consoles[0];
    else if (def->nserials)
        chr = def->serials[0];

    if (!chr) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("cannot find default console device"));
        goto cleanup;
    }

    if (chr->source.type != VIR_DOMAIN_CHR_TYPE_PTY) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("character device %s is not using a PTY"), dev_name);
        goto cleanup;
    }

    if (virFDStreamOpenFile(st, chr->source.data.file.path,
                            0, 0, O_RDWR) < 0)
        goto cleanup;

    ret = 0;
cleanup:
    virDomainDefFree(def);
    return ret;
}

static int
xenUnifiedNodeGetMemoryParameters(virConnectPtr conn ATTRIBUTE_UNUSED,
                                  virTypedParameterPtr params,
                                  int *nparams,
                                  unsigned int flags)
{
    return nodeGetMemoryParameters(params, nparams, flags);
}


static int
xenUnifiedNodeSetMemoryParameters(virConnectPtr conn ATTRIBUTE_UNUSED,
                                  virTypedParameterPtr params,
                                  int nparams,
                                  unsigned int flags)
{
    return nodeSetMemoryParameters(params, nparams, flags);
}


static int
xenUnifiedNodeSuspendForDuration(virConnectPtr conn ATTRIBUTE_UNUSED,
                                 unsigned int target,
                                 unsigned long long duration,
                                 unsigned int flags)
{
    return nodeSuspendForDuration(target, duration, flags);
}


/*----- Register with libvirt.c, and initialize Xen drivers. -----*/

/* The interface which we export upwards to libvirt.c. */
static virDriver xenUnifiedDriver = {
    .no = VIR_DRV_XEN_UNIFIED,
    .name = "Xen",
    .connectOpen = xenUnifiedConnectOpen, /* 0.0.3 */
    .connectClose = xenUnifiedConnectClose, /* 0.0.3 */
    .connectSupportsFeature = xenUnifiedConnectSupportsFeature, /* 0.3.2 */
    .connectGetType = xenUnifiedConnectGetType, /* 0.0.3 */
    .connectGetVersion = xenUnifiedConnectGetVersion, /* 0.0.3 */
    .connectGetHostname = xenUnifiedConnectGetHostname, /* 0.7.3 */
    .connectGetMaxVcpus = xenUnifiedConnectGetMaxVcpus, /* 0.2.1 */
    .nodeGetInfo = xenUnifiedNodeGetInfo, /* 0.1.0 */
    .connectGetCapabilities = xenUnifiedConnectGetCapabilities, /* 0.2.1 */
    .connectListDomains = xenUnifiedConnectListDomains, /* 0.0.3 */
    .connectNumOfDomains = xenUnifiedConnectNumOfDomains, /* 0.0.3 */
    .domainCreateXML = xenUnifiedDomainCreateXML, /* 0.0.3 */
    .domainLookupByID = xenUnifiedDomainLookupByID, /* 0.0.3 */
    .domainLookupByUUID = xenUnifiedDomainLookupByUUID, /* 0.0.5 */
    .domainLookupByName = xenUnifiedDomainLookupByName, /* 0.0.3 */
    .domainSuspend = xenUnifiedDomainSuspend, /* 0.0.3 */
    .domainResume = xenUnifiedDomainResume, /* 0.0.3 */
    .domainShutdown = xenUnifiedDomainShutdown, /* 0.0.3 */
    .domainShutdownFlags = xenUnifiedDomainShutdownFlags, /* 0.9.10 */
    .domainReboot = xenUnifiedDomainReboot, /* 0.1.0 */
    .domainDestroy = xenUnifiedDomainDestroy, /* 0.0.3 */
    .domainDestroyFlags = xenUnifiedDomainDestroyFlags, /* 0.9.4 */
    .domainGetOSType = xenUnifiedDomainGetOSType, /* 0.0.3 */
    .domainGetMaxMemory = xenUnifiedDomainGetMaxMemory, /* 0.0.3 */
    .domainSetMaxMemory = xenUnifiedDomainSetMaxMemory, /* 0.0.3 */
    .domainSetMemory = xenUnifiedDomainSetMemory, /* 0.1.1 */
    .domainGetInfo = xenUnifiedDomainGetInfo, /* 0.0.3 */
    .domainGetState = xenUnifiedDomainGetState, /* 0.9.2 */
    .domainSave = xenUnifiedDomainSave, /* 0.0.3 */
    .domainSaveFlags = xenUnifiedDomainSaveFlags, /* 0.9.4 */
    .domainManagedSave = xenUnifiedDomainManagedSave, /* 1.0.1 */
    .domainHasManagedSaveImage = xenUnifiedDomainHasManagedSaveImage, /* 1.0.1 */
    .domainManagedSaveRemove = xenUnifiedDomainManagedSaveRemove, /* 1.0.1 */
    .domainRestore = xenUnifiedDomainRestore, /* 0.0.3 */
    .domainRestoreFlags = xenUnifiedDomainRestoreFlags, /* 0.9.4 */
    .domainCoreDump = xenUnifiedDomainCoreDump, /* 0.1.9 */
    .domainSetVcpus = xenUnifiedDomainSetVcpus, /* 0.1.4 */
    .domainSetVcpusFlags = xenUnifiedDomainSetVcpusFlags, /* 0.8.5 */
    .domainGetVcpusFlags = xenUnifiedDomainGetVcpusFlags, /* 0.8.5 */
    .domainPinVcpu = xenUnifiedDomainPinVcpu, /* 0.1.4 */
    .domainGetVcpus = xenUnifiedDomainGetVcpus, /* 0.1.4 */
    .domainGetMaxVcpus = xenUnifiedDomainGetMaxVcpus, /* 0.2.1 */
    .domainGetXMLDesc = xenUnifiedDomainGetXMLDesc, /* 0.0.3 */
    .connectDomainXMLFromNative = xenUnifiedConnectDomainXMLFromNative, /* 0.6.4 */
    .connectDomainXMLToNative = xenUnifiedConnectDomainXMLToNative, /* 0.6.4 */
    .connectListDefinedDomains = xenUnifiedConnectListDefinedDomains, /* 0.1.1 */
    .connectNumOfDefinedDomains = xenUnifiedConnectNumOfDefinedDomains, /* 0.1.5 */
    .domainCreate = xenUnifiedDomainCreate, /* 0.1.1 */
    .domainCreateWithFlags = xenUnifiedDomainCreateWithFlags, /* 0.8.2 */
    .domainDefineXML = xenUnifiedDomainDefineXML, /* 0.1.1 */
    .domainUndefine = xenUnifiedDomainUndefine, /* 0.1.1 */
    .domainUndefineFlags = xenUnifiedDomainUndefineFlags, /* 0.9.4 */
    .domainAttachDevice = xenUnifiedDomainAttachDevice, /* 0.1.9 */
    .domainAttachDeviceFlags = xenUnifiedDomainAttachDeviceFlags, /* 0.7.7 */
    .domainDetachDevice = xenUnifiedDomainDetachDevice, /* 0.1.9 */
    .domainDetachDeviceFlags = xenUnifiedDomainDetachDeviceFlags, /* 0.7.7 */
    .domainUpdateDeviceFlags = xenUnifiedDomainUpdateDeviceFlags, /* 0.8.0 */
    .domainGetAutostart = xenUnifiedDomainGetAutostart, /* 0.4.4 */
    .domainSetAutostart = xenUnifiedDomainSetAutostart, /* 0.4.4 */
    .domainGetSchedulerType = xenUnifiedDomainGetSchedulerType, /* 0.2.3 */
    .domainGetSchedulerParameters = xenUnifiedDomainGetSchedulerParameters, /* 0.2.3 */
    .domainGetSchedulerParametersFlags = xenUnifiedDomainGetSchedulerParametersFlags, /* 0.9.2 */
    .domainSetSchedulerParameters = xenUnifiedDomainSetSchedulerParameters, /* 0.2.3 */
    .domainSetSchedulerParametersFlags = xenUnifiedDomainSetSchedulerParametersFlags, /* 0.9.2 */
    .domainMigratePrepare = xenUnifiedDomainMigratePrepare, /* 0.3.2 */
    .domainMigratePerform = xenUnifiedDomainMigratePerform, /* 0.3.2 */
    .domainMigrateFinish = xenUnifiedDomainMigrateFinish, /* 0.3.2 */
    .domainBlockStats = xenUnifiedDomainBlockStats, /* 0.3.2 */
    .domainInterfaceStats = xenUnifiedDomainInterfaceStats, /* 0.3.2 */
    .domainBlockPeek = xenUnifiedDomainBlockPeek, /* 0.4.4 */
    .nodeGetCellsFreeMemory = xenUnifiedNodeGetCellsFreeMemory, /* 0.3.3 */
    .nodeGetFreeMemory = xenUnifiedNodeGetFreeMemory, /* 0.3.3 */
    .connectDomainEventRegister = xenUnifiedConnectDomainEventRegister, /* 0.5.0 */
    .connectDomainEventDeregister = xenUnifiedConnectDomainEventDeregister, /* 0.5.0 */
    .nodeDeviceDettach = xenUnifiedNodeDeviceDettach, /* 0.6.1 */
    .nodeDeviceDetachFlags = xenUnifiedNodeDeviceDetachFlags, /* 1.0.5 */
    .nodeDeviceReAttach = xenUnifiedNodeDeviceReAttach, /* 0.6.1 */
    .nodeDeviceReset = xenUnifiedNodeDeviceReset, /* 0.6.1 */
    .connectIsEncrypted = xenUnifiedConnectIsEncrypted, /* 0.7.3 */
    .connectIsSecure = xenUnifiedConnectIsSecure, /* 0.7.3 */
    .domainIsActive = xenUnifiedDomainIsActive, /* 0.7.3 */
    .domainIsPersistent = xenUnifiedDomainIsPersistent, /* 0.7.3 */
    .domainIsUpdated = xenUnifiedDomainIsUpdated, /* 0.8.6 */
    .connectDomainEventRegisterAny = xenUnifiedConnectDomainEventRegisterAny, /* 0.8.0 */
    .connectDomainEventDeregisterAny = xenUnifiedConnectDomainEventDeregisterAny, /* 0.8.0 */
    .domainOpenConsole = xenUnifiedDomainOpenConsole, /* 0.8.6 */
    .connectIsAlive = xenUnifiedConnectIsAlive, /* 0.9.8 */
    .nodeSuspendForDuration = xenUnifiedNodeSuspendForDuration, /* 0.9.8 */
    .nodeGetMemoryParameters = xenUnifiedNodeGetMemoryParameters, /* 0.10.2 */
    .nodeSetMemoryParameters = xenUnifiedNodeSetMemoryParameters, /* 0.10.2 */
};

/**
 * xenRegister:
 *
 * Register xen related drivers
 *
 * Returns the driver priority or -1 in case of error.
 */
int
xenRegister(void)
{
    if (virRegisterStateDriver(&state_driver) == -1) return -1;

    return virRegisterDriver(&xenUnifiedDriver);
}

/**
 * xenUnifiedDomainInfoListFree:
 *
 * Free the Domain Info List
 */
void
xenUnifiedDomainInfoListFree(xenUnifiedDomainInfoListPtr list)
{
    int i;

    if (list == NULL)
        return;

    for (i=0; i<list->count; i++) {
        VIR_FREE(list->doms[i]->name);
        VIR_FREE(list->doms[i]);
    }
    VIR_FREE(list->doms);
    VIR_FREE(list);
}

/**
 * xenUnifiedAddDomainInfo:
 *
 * Add name and uuid to the domain info list
 *
 * Returns: 0 on success, -1 on failure
 */
int
xenUnifiedAddDomainInfo(xenUnifiedDomainInfoListPtr list,
                        int id, char *name,
                        unsigned char *uuid)
{
    xenUnifiedDomainInfoPtr info;
    int n;

    /* check if we already have this callback on our list */
    for (n=0; n < list->count; n++) {
        if (STREQ(list->doms[n]->name, name) &&
            !memcmp(list->doms[n]->uuid, uuid, VIR_UUID_BUFLEN)) {
            VIR_DEBUG("WARNING: dom already tracked");
            return -1;
        }
    }

    if (VIR_ALLOC(info) < 0)
        goto memory_error;
    if (!(info->name = strdup(name)))
        goto memory_error;

    memcpy(info->uuid, uuid, VIR_UUID_BUFLEN);
    info->id = id;

    /* Make space on list */
    n = list->count;
    if (VIR_REALLOC_N(list->doms, n + 1) < 0) {
        goto memory_error;
    }

    list->doms[n] = info;
    list->count++;
    return 0;
memory_error:
    virReportOOMError();
    if (info)
        VIR_FREE(info->name);
    VIR_FREE(info);
    return -1;
}

/**
 * xenUnifiedRemoveDomainInfo:
 *
 * Removes name and uuid to the domain info list
 *
 * Returns: 0 on success, -1 on failure
 */
int
xenUnifiedRemoveDomainInfo(xenUnifiedDomainInfoListPtr list,
                           int id, char *name,
                           unsigned char *uuid)
{
    int i;
    for (i = 0 ; i < list->count ; i++) {
        if (list->doms[i]->id == id &&
            STREQ(list->doms[i]->name, name) &&
            !memcmp(list->doms[i]->uuid, uuid, VIR_UUID_BUFLEN)) {

            VIR_FREE(list->doms[i]->name);
            VIR_FREE(list->doms[i]);

            if (i < (list->count - 1))
                memmove(list->doms + i,
                        list->doms + i + 1,
                        sizeof(*(list->doms)) *
                                (list->count - (i + 1)));

            if (VIR_REALLOC_N(list->doms,
                              list->count - 1) < 0) {
                ; /* Failure to reduce memory allocation isn't fatal */
            }
            list->count--;

            return 0;
        }
    }
    return -1;
}


/**
 * xenUnifiedDomainEventDispatch:
 * @priv: the connection to dispatch events on
 * @event: the event to dispatch
 *
 * Dispatch domain events to registered callbacks
 *
 * The caller must hold the lock in 'priv' before invoking
 *
 */
void xenUnifiedDomainEventDispatch(xenUnifiedPrivatePtr priv,
                                    virDomainEventPtr event)
{
    if (!priv)
        return;

    virDomainEventStateQueue(priv->domainEvents, event);
}

void xenUnifiedLock(xenUnifiedPrivatePtr priv)
{
    virMutexLock(&priv->lock);
}

void xenUnifiedUnlock(xenUnifiedPrivatePtr priv)
{
    virMutexUnlock(&priv->lock);
}

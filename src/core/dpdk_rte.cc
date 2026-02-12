/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#ifdef SEASTAR_HAVE_DPDK

#include <cinttypes>
#include <cstdlib>
#include <sstream>
#include <seastar/net/dpdk.hh>
#include <seastar/core/dpdk_rte.hh>
#include <seastar/util/conversions.hh>
#include <seastar/util/log.hh>
#include <seastar/util/std-compat.hh>
#include <rte_pci.h>

namespace seastar {

extern seastar::logger seastar_logger;

namespace dpdk {

bool eal::initialized = false;

void eal::init(cpuset cpus, const std::string& argv0, const std::optional<std::string>& hugepages_path, bool dpdk_pmd)
{
    if (initialized) {
        return;
    }

    size_t cpu_count = cpus.count();
    std::stringstream mask;
    cpuset nibble = 0xF;
    while (cpus.any()) {
        mask << std::hex << (cpus & nibble).to_ulong();
        cpus >>= 4;
    }

    std::string mask_str = mask.str();
    std::reverse(mask_str.begin(), mask_str.end());

    std::string mem_channels = "1";
    if (const char* mem_channels_env = std::getenv("SEASTAR_DPDK_MEM_CHANNELS")) {
        char* end = nullptr;
        unsigned long parsed_mem_channels = std::strtoul(mem_channels_env, &end, 10);
        if (*mem_channels_env != '\0' && *end == '\0' && parsed_mem_channels > 0) {
            mem_channels = std::to_string(parsed_mem_channels);
        } else {
            seastar_logger.warn("Ignoring invalid SEASTAR_DPDK_MEM_CHANNELS='{}', using default {}", mem_channels_env, mem_channels);
        }
    }

    std::vector<std::vector<char>> args {
        string2vector(argv0),
        string2vector("-c"), string2vector(mask_str),
        string2vector("-n"), string2vector(mem_channels)
    };

    // If "hugepages" is not provided and DPDK PMD drivers mode is requested -
    // use the default DPDK huge tables configuration.
    if (hugepages_path) {
        args.push_back(string2vector("--huge-dir"));
        args.push_back(string2vector(hugepages_path.value()));

        //
        // We don't know what is going to be our networking configuration so we
        // assume there is going to be a queue per-CPU. Plus we'll give a DPDK
        // 64MB for "other stuff".
        //
        size_t size_MB = mem_size(cpu_count) >> 20;
        std::stringstream size_MB_str;
        size_MB_str << size_MB;

        args.push_back(string2vector("-m"));
        args.push_back(string2vector(size_MB_str.str()));
    } else if (!dpdk_pmd) {
        args.push_back(string2vector("--no-huge"));
    }

    // Allow additional DPDK EAL arguments via environment variable.
    // Format: SEASTAR_DPDK_EXTRA_ARGS="--log-level=lib.eal:debug -a 0000:03:00.0"
    if (const char* eal_args = std::getenv("SEASTAR_DPDK_EXTRA_ARGS")) {
        std::stringstream ss(eal_args);
        std::string arg;
        while (ss >> arg) {
            args.push_back(string2vector(arg));
        }
    }

    std::vector<char*> cargs;

    for (auto&& a: args) {
        cargs.push_back(a.data());
    }

    seastar_logger.info("Initializing DPDK EAL with arguments: {}", fmt::join(cargs, " "));

    /* initialise the EAL for all */
    int ret = rte_eal_init(cargs.size(), cargs.data());
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot init EAL\n");
    }

    initialized = true;
}

uint32_t __attribute__((weak)) qp_mempool_obj_size(bool hugetlbfs_membackend)
{
    return 0;
}

size_t eal::mem_size(int num_cpus, bool hugetlbfs_membackend)
{
    size_t memsize = 0;
    //
    // PMD mempool memory:
    //
    // We don't know what is going to be our networking configuration so we
    // assume there is going to be a queue per-CPU.
    //
    memsize += num_cpus * qp_mempool_obj_size(hugetlbfs_membackend);

    // Plus we'll give a DPDK 64MB for "other stuff".
    memsize += (64UL << 20);

    return memsize;
}

} // namespace dpdk

}

#endif // SEASTAR_HAVE_DPDK

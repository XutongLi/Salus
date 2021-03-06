# -*- coding: future_fstrings -*-
#
# Copyright 2019 Peifeng Yu <peifeng@umich.edu>
# 
# This file is part of Salus
# (see https://github.com/SymbioticLab/Salus).
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# 
#    http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
"""
For debugging. Run one large job repeatly, to see if the GPU memory can be completely freed between sessions.

Scheduler: fair
Work conservation: True
Collected data: memory usage over time
"""
from __future__ import absolute_import, print_function, division, unicode_literals

from absl import flags

from benchmarks.driver.server.config import presets
from benchmarks.driver.workload import WTL
from benchmarks.exps import run_seq, parse_actions_from_cmd, maybe_forced_preset, Pause

from itertools import chain


FLAGS = flags.FLAGS


def main(argv):
    scfg = maybe_forced_preset(presets.AllocProf)
    if argv:
        run_seq(scfg.copy(output_dir=FLAGS.save_dir),
                *parse_actions_from_cmd(argv))
        return

    def create_wl():
        return WTL.create("inception4", 25, 200)

    seq = [
        create_wl(),
        create_wl(),
        create_wl(),
        Pause.Wait,
        create_wl(),
        Pause.Wait,
        create_wl(),
        create_wl(),
        Pause.Wait,
        create_wl(),
        create_wl(),
        create_wl(),
        Pause.Wait,
    ]

    run_seq(scfg.copy(output_dir=FLAGS.save_dir), *seq)

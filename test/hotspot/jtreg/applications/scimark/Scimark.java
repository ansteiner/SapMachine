/*
 * Copyright (c) 2017, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * @test
 * @key external-dep
 * @library /test/lib
 * @run driver Scimark
 */

import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.artifacts.Artifact;
import jdk.test.lib.artifacts.ArtifactResolver;
import jdk.test.lib.artifacts.ArtifactResolverException;
import java.nio.file.Path;
import java.util.Map;

@Artifact(organization = "gov.nist.math", name = "scimark", revision = "2.0", extension = "zip")
public class Scimark {
    public static void main(String... args) throws Exception {
        // SapMachine 2018-07-06: Prefer Scimark classpath from system property.
        String sciMark2Cp = System.getProperty("SCIMARK_2_CP");
        if (sciMark2Cp == null) {
            Map<String, Path> artifacts;
            try {
                artifacts  = ArtifactResolver.resolve(Scimark.class);
            } catch (ArtifactResolverException e) {
                throw new Error("TESTBUG: Can not resolve artifacts for "
                                + Scimark.class.getName(), e);
            }
            sciMark2Cp = artifacts.get("gov.nist.math.scimark-2.0").toString();
        }

        System.setProperty("test.noclasspath", "true");

        OutputAnalyzer output = new OutputAnalyzer(ProcessTools.createTestJavaProcessBuilder(
            "-cp", sciMark2Cp,
            "jnt.scimark2.commandline", "-large")
            .start());
        output.shouldHaveExitValue(0);
    }
}

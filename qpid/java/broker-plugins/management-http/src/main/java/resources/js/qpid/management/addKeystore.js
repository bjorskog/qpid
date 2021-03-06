/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */
define(["dojo/_base/lang",
        "dojo/_base/xhr",
        "dojo/dom",
        "dojo/dom-construct",
        "dijit/registry",
        "dojo/parser",
        "dojo/_base/array",
        "dojo/_base/event",
        'dojo/_base/json',
        "qpid/common/util",
        "dojo/store/Memory",
        "dojox/validate/us",
        "dojox/validate/web",
        "dijit/Dialog",
        "dijit/form/CheckBox",
        "dijit/form/Textarea",
        "dijit/form/ComboBox",
        "dijit/form/TextBox",
        "dijit/form/ValidationTextBox",
        "dijit/form/Button",
        "dijit/form/Form",
        "dijit/TitlePane",
        "dojox/layout/TableContainer",
        "dojo/domReady!"],
    function (lang, xhr, dom, construct, registry, parser, array, event, json, util) {

        var addKeystore = { };

        addKeystore.createWidgetFactories = function(isKeystore)
        {
            var fields = [{
                name: "name",
                createWidget: function(keystore) {
                    return new dijit.form.ValidationTextBox({
                      required: true,
                      value: keystore.name,
                      disabled: keystore.name ? true : false,
                      label: "Name:",
                      regExpGen: util.nameOrContextVarRegexp,
                      promptMessage: "Name of keystore. Used to refer to the keystore from other objects within the Broker.",
                      placeHolder: "name",
                      name: "name"});
                }
            }, {
                name: "path",
                createWidget: function(keystore) {
                    return new dijit.form.ValidationTextBox({
                      required: true,
                      value: keystore.path,
                      label: "Path to keystore:",
                      promptMessage: "File system location to the keystore file",
                      placeHolder: "path/to/keystore",
                      name: "path"});
                }
            }, {
                name: "password",
                requiredFor: "path",
                createWidget: function(keystore) {
                    return new dijit.form.ValidationTextBox({
                      required: false,
                      label: "Keystore password:",
                      promptMessage: "Password used to open the keystore",
                      name: "password",
                      placeHolder: keystore["password"] ? keystore["password"] : ""
                      });
                }
            }];
            if (!isKeystore)
            {
              fields.push({
                name: "peersOnly",
                createWidget: function(keystore) {
                    return new dijit.form.CheckBox({
                      required: false,
                      checked: keystore && keystore.peersOnly,
                      label: "Peers only:",
                      name: "peersOnly"});
                }
              });
            }
            fields.push({
              name: "Options",

              createWidget: function(keystore) {
                var optionalFieldContainer = new dojox.layout.TableContainer({
                  cols: 1,
                  "labelWidth": "300",
                  showLabels: true,
                  orientation: "horiz",
                  customClass: "formLabel"
                });
                if (isKeystore)
                {
                  optionalFieldContainer.addChild(new dijit.form.ValidationTextBox({
                    required: false,
                    value: keystore.certificateAlias,
                    label: "Keystore certificate alias:",
                    name: "certificateAlias",
                    placeHolder: "alias",
                    promptMessage: "Used to identify one certificate in a store that has many"}));

                  optionalFieldContainer.addChild( new dijit.form.ValidationTextBox({
                    required: false,
                    value: keystore.keyManagerFactoryAlgorithm,
                    label: "Key manager factory algorithm:",
                    placeHolder: "algorithm name",
                    promptMessage: "Name of the key manager algorithm known to Java",
                    name: "keyManagerFactoryAlgorithm"}));
                }
                else
                {
                  optionalFieldContainer.addChild( new dijit.form.ValidationTextBox({
                    required: false,
                    value: keystore.trustManagerFactoryAlgorithm,
                    label: "Trust manager factory algorithm:",
                    placeHolder: "algorithm name",
                    promptMessage: "Name of the trust manager algorithm known to Java",
                    name: "trustManagerFactoryAlgorithm"}));
                }
                optionalFieldContainer.addChild(new dijit.form.ValidationTextBox({
                  required: false,
                  value: isKeystore ? keystore.keyStoreType : keystore.trustStoreType,
                  label: "Key store type:",
                  placeHolder: "store type",
                  promptMessage: "Name of the store type known to Java",
                  name: isKeystore ? "keyStoreType" : "trustStoreType"}));

                var panel = new dijit.TitlePane({title: "Optional Attributes", content: optionalFieldContainer.domNode, open: false});

                return panel;
              }
            });
            return fields;
        }

        addKeystore.showKeystoreDialog = function(keystore, putURL) {
          var keystoreAttributeWidgetFactories = addKeystore.createWidgetFactories(true);

          util.showSetAttributesDialog(
              keystoreAttributeWidgetFactories,
              keystore ? keystore : {},
              keystore ? putURL : "api/latest/keystore",
              keystore ? "Edit keystore - " + keystore.name : "Add keystore",
              "KeyStore",
              keystore && keystore.type ? keystore.type : "FileKeyStore",  // GET?actuals=true doesn't get type for objects of the default type for the category
              keystore ? false : true);
        };

        addKeystore.showTruststoreDialog = function(truststore, putURL) {
          var truststoreAttributeWidgetFactories = addKeystore.createWidgetFactories(false);
          util.showSetAttributesDialog(
              truststoreAttributeWidgetFactories,
              truststore ? truststore : {},
              truststore ? putURL : "api/latest/truststore",
              truststore ? "Edit truststore - " + truststore.name : "Add truststore",
              "TrustStore",
              truststore && truststore.type ? truststore.type : "FileTrustStore",
              truststore ? false : true);
        };
        return addKeystore;
    });
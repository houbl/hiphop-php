<?php
include "test_schema.inc";
$schema = <<<EOF
	<complexType name="testType2">
		<sequence>
			<element name="int" type="int"/>
		</sequence>
	</complexType>
	<complexType name="testType">
		<complexContent>
			<extension base="tns:testType2">
				<attribute name="int2" type="int"/>
			</extension>
		</complexContent>
	</complexType>
EOF;
test_schema($schema,'type="tns:testType"',(object)array("_"=>123.5,"int"=>123.5,"int2"=>123.5));
echo "ok";
?>